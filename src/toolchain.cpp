#include "toolchain.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common.h"
#include "fsops.h"
#include "proc.h"

// CMake 用 `ld -r -b binary` 把 prebuilt/<abi>/{tar,zstd} 嵌进可执行文件，
// 生成的符号固定为 _binary_<file>_start / _binary_<file>_end。
extern "C" {
extern const unsigned char _binary_tar_start[];
extern const unsigned char _binary_tar_end[];
extern const unsigned char _binary_zstd_start[];
extern const unsigned char _binary_zstd_end[];
}

namespace appbackup {
namespace toolchain {
namespace {

std::string sh_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string lower(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return o;
}

std::string g_exe_dir;
bool g_exe_dir_done = false;

std::string exe_dir() {
    if (g_exe_dir_done) return g_exe_dir;
    g_exe_dir_done = true;
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        g_exe_dir = dirname_of(buf);
    }
    return g_exe_dir;
}

// 真执行一次来判断可用性：noexec 挂载、ABI 不符都会在 exec 阶段失败。
// 这里必须自己 fork+execl 并用 CLOEXEC 管道判定，不能复用 run_exec：execvp 在遇到
// ENOEXEC 时会回退到 /bin/sh 执行该文件（POSIX 历史行为），会把 ABI 不符的二进制
// 误判成「能跑」。
bool can_run(const std::string& p) {
    if (p.empty()) return false;
    struct stat st;
    if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (access(p.c_str(), X_OK) != 0) return false;

    int errpipe[2];
    if (pipe(errpipe) != 0) return false;
    fcntl(errpipe[1], F_SETFD, FD_CLOEXEC);  // exec 成功后自动关闭 → 父进程读到 EOF

    pid_t pid = fork();
    if (pid == 0) {
        close(errpipe[0]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {
            dup2(dn, STDOUT_FILENO);
            dup2(dn, STDERR_FILENO);
            close(dn);
        }
        execl(p.c_str(), p.c_str(), "--version", static_cast<char*>(nullptr));
        int e = errno;
        ssize_t w = write(errpipe[1], &e, sizeof(e));
        (void)w;
        _exit(127);
    }
    close(errpipe[1]);
    if (pid < 0) {
        close(errpipe[0]);
        return false;
    }
    int exec_errno = 0;
    ssize_t n = read(errpipe[0], &exec_errno, sizeof(exec_errno));
    close(errpipe[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return n <= 0;  // 读到 errno 说明 exec 失败
}

bool write_blob(const std::string& path, const unsigned char* data, size_t len) {
    std::string tmp = path + ".tmp." + std::to_string(static_cast<long>(getpid()));
    unlink(tmp.c_str());
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) {
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        off += static_cast<size_t>(n);
    }
    fsync(fd);
    close(fd);
    if (chmod(tmp.c_str(), 0755) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

std::vector<std::string> release_dirs() {
    std::vector<std::string> v;
    const char* env = getenv("APPBACKUP_BIN_DIR");
    if (env && *env) v.push_back(env);
    std::string ed = exe_dir();
    if (!ed.empty()) {
        v.push_back(join_path(ed, "bin"));  // 与可执行文件同级的 bin/
        v.push_back(ed);                    // 或直接同目录
    }
    v.push_back("/data/local/tmp/.appbackup-bin");  // 一定可执行，且重启不丢
    v.push_back("/data/adb/appbackup/bin");         // Magisk/KernelSU 模块布局
    return v;
}

// 释放内嵌二进制；返回可用路径，全部失败返回空串
std::string ensure_tool(const char* name, const unsigned char* begin, const unsigned char* end) {
    const size_t len = static_cast<size_t>(end - begin);
    if (len == 0) return std::string();

    // 0) APPBACKUP_BIN_DIR 是显式指定：里面已有可执行就用它，绝不用内嵌副本覆盖。
    //    （既是运维手段，也是测试里模拟「受限 tar」的注入点）
    const char* env = getenv("APPBACKUP_BIN_DIR");
    if (env && *env) {
        std::string p = join_path(env, name);
        if (can_run(p)) return p;
    }

    // 1) 已有完整副本且真能跑 → 直接用，避免每次运行重写 2.5MB
    for (const auto& d : release_dirs()) {
        std::string p = join_path(d, name);
        if (file_size(p) == len && can_run(p)) return p;
    }
    // 2) 释放到第一个可写、且释放后真的能执行的目录
    for (const auto& d : release_dirs()) {
        if (!make_dirs(d, 0755)) continue;
        std::string p = join_path(d, name);
        if (!write_blob(p, begin, len)) continue;
        if (can_run(p)) {
            log_debug("[toolchain] released %s -> %s (%zu bytes)", name, p.c_str(), len);
            return p;
        }
        log_warn("[toolchain] 释放后无法执行（noexec 挂载或 ABI 不符）: %s", p.c_str());
        unlink(p.c_str());
    }
    return std::string();
}

std::string g_tar;
std::string g_zstd;
bool g_tar_done = false;
bool g_zstd_done = false;

std::string resolve_tar() {
    if (g_tar_done) return g_tar;
    g_tar_done = true;
    g_tar = ensure_tool("tar", _binary_tar_start, _binary_tar_end);
    if (g_tar.empty()) {
        log_warn("[toolchain] 内嵌 tar 释放失败，回退系统 tar（GNU 专有选项会自动降级）");
        g_tar = "tar";  // 交给 execvp 走 PATH
    }
    return g_tar;
}

std::string resolve_zstd() {
    if (g_zstd_done) return g_zstd;
    g_zstd_done = true;
    g_zstd = ensure_tool("zstd", _binary_zstd_start, _binary_zstd_end);
    if (g_zstd.empty()) {
        log_warn("[toolchain] 内嵌 zstd 释放失败，回退系统 zstd");
        g_zstd = "zstd";
    }
    return g_zstd;
}

struct Caps {
    bool done = false;
    bool totals = false;
    bool exclude = false;
    bool follow = false;
    bool recursive_unlink = false;
};
Caps g_caps;

// 按「选项是否被拒绝」判断，而不是退出码：GNU tar 读 /dev/null 也会返回非 0，
// 但那不代表选项不受支持。
bool probe_flag(const std::string& opt) {
    ExecResult r = run_shell(sh_quote(tar()) + " " + opt + " -tf /dev/null 2>&1");
    const std::string o = lower(r.out);
    static const char* kReject[] = {
        "unrecognized option", "unknown option", "invalid option",
        "illegal option",      "not supported",  "unrecognized",
    };
    for (const char* k : kReject) {
        if (o.find(k) != std::string::npos) return false;
    }
    return true;
}

void probe_all() {
    if (g_caps.done) return;
    g_caps.done = true;
    g_caps.totals = probe_flag("--totals");
    g_caps.exclude = probe_flag("--exclude=__appbackup_probe__");
    g_caps.follow = probe_flag("-h");
    g_caps.recursive_unlink = probe_flag("--recursive-unlink");
    log_info("[toolchain] tar=%s totals=%d exclude=%d -h=%d recursive-unlink=%d", tar().c_str(),
             g_caps.totals ? 1 : 0, g_caps.exclude ? 1 : 0, g_caps.follow ? 1 : 0,
             g_caps.recursive_unlink ? 1 : 0);
    log_info("[toolchain] zstd=%s", zstd().c_str());
}

}  // namespace

std::string tar() { return resolve_tar(); }
std::string zstd() { return resolve_zstd(); }

bool tar_has_totals() { probe_all(); return g_caps.totals; }
bool tar_has_exclude() { probe_all(); return g_caps.exclude; }
bool tar_has_follow_symlinks() { probe_all(); return g_caps.follow; }
bool tar_has_recursive_unlink() { probe_all(); return g_caps.recursive_unlink; }

std::string describe() {
    probe_all();
    return "tar=" + tar() + " totals=" + (g_caps.totals ? "1" : "0") +
           " exclude=" + (g_caps.exclude ? "1" : "0") + " -h=" + (g_caps.follow ? "1" : "0") +
           " recursive-unlink=" + (g_caps.recursive_unlink ? "1" : "0") + " zstd=" + zstd();
}

void reset_cache() {
    g_exe_dir.clear();
    g_exe_dir_done = false;
    g_tar.clear();
    g_tar_done = false;
    g_zstd.clear();
    g_zstd_done = false;
    g_caps = Caps{};
}

}  // namespace toolchain
}  // namespace appbackup
