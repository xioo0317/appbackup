#include "sysarchive.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <cstring>

#include "common.h"
#include "fsops.h"
#include "proc.h"
#include "toolchain.h"

namespace appbackup {

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

// 列 srcDir 顶层的 *.apk（字典序，与 shell glob 展开顺序一致）。
// 上游写的是 `tar ... ./*.apk`，由 shell 展开；这里自己列，避免「目录里没有 apk 时
// glob 不展开、tar 直接报 Cannot stat」的坑。
std::vector<std::string> list_top_level_apks(const std::string& dir) {
    std::vector<std::string> v;
    DIR* d = opendir(dir.c_str());
    if (!d) return v;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n.size() <= 4 || !ends_with(n, ".apk")) continue;
        struct stat st;
        if (stat(join_path(dir, n).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        v.push_back(n);
    }
    closedir(d);
    std::sort(v.begin(), v.end());
    return v;
}

// 解析 GNU tar --totals 的 "Total bytes written: N"
uint64_t parse_totals(const std::string& out) {
    static const char kKey[] = "Total bytes written:";
    size_t pos = out.find(kKey);
    if (pos == std::string::npos) return 0;
    pos += sizeof(kKey) - 1;
    while (pos < out.size() && (out[pos] == ' ' || out[pos] == '\t')) pos++;
    uint64_t v = 0;
    while (pos < out.size() && out[pos] >= '0' && out[pos] <= '9') {
        v = v * 10 + static_cast<uint64_t>(out[pos] - '0');
        pos++;
    }
    return v;
}

bool g_zstd_priority_checked = false;
bool g_zstd_priority_ok = false;

// 上游带 --priority=rt（实时调度）；个别 zstd 构建没有该选项，探测一次后缓存。
bool zstd_supports_priority_rt() {
    if (g_zstd_priority_checked) return g_zstd_priority_ok;
    g_zstd_priority_checked = true;
    ExecResult r = run_shell(sh_quote(toolchain::zstd()) + " --help 2>&1");
    g_zstd_priority_ok = (r.out.find("priority") != std::string::npos);
    return g_zstd_priority_ok;
}

// 上游压缩参数：zstd -r -T0 --ultra -q [--priority=rt] -<level>
std::string zstd_cmd(int level, bool with_priority) {
    std::string c = sh_quote(toolchain::zstd()) + " -r -T0 --ultra -q";
    if (with_priority && zstd_supports_priority_rt()) c += " --priority=rt";
    c += " -" + std::to_string(level);
    return c;
}

}  // namespace

bool sys_backup(const SysBackupOpt& o, uint64_t* uncompressed_bytes) {
    if (uncompressed_bytes) *uncompressed_bytes = 0;

    // 先写 .part 再 rename：中途失败不会把上一份好归档截断
    const std::string part = o.dst + ".part";
    unlink(part.c_str());

    std::string tarcmd = sh_quote(toolchain::tar());
    if (toolchain::tar_has_totals()) tarcmd += " --totals";

    std::string srcs;
    if (o.apk_mode) {
        std::vector<std::string> apks = list_top_level_apks(o.srcDir);
        if (apks.empty()) {
            log_info("[apk] 目录内没有 .apk，跳过: %s", o.srcDir.c_str());
            if (o.skipped) *o.skipped = true;
            return true;
        }
        for (const auto& n : apks) srcs += " " + sh_quote("./" + n);
    } else {
        if (!o.excludes.empty()) {
            if (toolchain::tar_has_exclude()) {
                for (const auto& e : o.excludes) tarcmd += " --exclude=" + sh_quote(e);
            } else {
                log_warn("[tar] 当前 tar 不支持 --exclude，排除项被忽略（归档变大，内容仍完整）");
            }
        }
        if (o.follow_symlinks && toolchain::tar_has_follow_symlinks()) tarcmd += " -h";
        tarcmd += " -C " + sh_quote(o.srcDir);
        srcs = " -- " + sh_quote(o.src);
    }

    // 管道里 tar 的退出码会被右侧压缩器掩盖。这里不依赖 `set -o pipefail`
    // （dash 遇到它会直接退出整个 shell），而是让左端把 tar 的退出码写进临时文件，
    // 之后单独校验——任何 POSIX sh 都能工作。
    const std::string rcf = part + ".rc";
    unlink(rcf.c_str());

    std::string cmd;
    if (o.apk_mode) cmd += "cd " + sh_quote(o.srcDir) + " || exit 1; ";

    if (o.compression == Compression::Plain) {
        cmd += tarcmd + " -cpf " + sh_quote(part) + srcs;
        cmd += "; _rc=$?; ";
        if (o.apk_mode) cmd += "cd /; ";
        cmd += "exit $_rc";
    } else {
        cmd += "{ " + tarcmd + " -cpf -" + srcs + "; echo $? > " + sh_quote(rcf) + "; } | ";
        if (o.compression == Compression::Zstd) {
            cmd += zstd_cmd(o.level, true);
        } else {
            cmd += "gzip -" + std::to_string(o.level);
        }
        // 重定向到文件（不是 zstd -o）：stdin→stdout 时帧头不写 contentSize，与上游一致
        cmd += " > " + sh_quote(part);
        cmd += "; _rc=$?; ";
        if (o.apk_mode) cmd += "cd /; ";
        cmd += "exit $_rc";
    }

    ExecResult r = run_shell(cmd);

    int tar_rc = 0;
    if (o.compression != Compression::Plain) {
        const std::string s = trim(read_file_head(rcf, 32));
        tar_rc = s.empty() ? -1 : atoi(s.c_str());
        unlink(rcf.c_str());
    }

    if (tar_rc != 0) {
        log_error("[tar] 打包失败 (tar code=%d): %s", tar_rc, trim(r.out).c_str());
        unlink(part.c_str());
        return false;
    }
    if (r.code != 0) {
        log_error("[tar] 压缩失败 (code=%d): %s", r.code, trim(r.out).c_str());
        unlink(part.c_str());
        return false;
    }
    if (file_size(part) == 0) {
        log_error("[tar] 未产出归档: %s", part.c_str());
        unlink(part.c_str());
        return false;
    }
    if (uncompressed_bytes) *uncompressed_bytes = parse_totals(r.out);

    if (rename(part.c_str(), o.dst.c_str()) != 0) {
        log_error("[tar] rename 失败 %s: %s", o.dst.c_str(), strerror(errno));
        unlink(part.c_str());
        return false;
    }
    return true;
}

bool sys_restore(const SysRestoreOpt& o) {
    // 目标目录必须存在（tar -C 不自建）；上游恢复前同样先 mkdirs
    if (!path_exists(o.dstDir) && !make_dirs(o.dstDir, 0755)) {
        log_error("[tar] 无法创建目标目录: %s (%s)", o.dstDir.c_str(), strerror(errno));
        return false;
    }

    std::string tarcmd = sh_quote(toolchain::tar());
    if (toolchain::tar_has_totals()) tarcmd += " --totals";
    if (!o.excludes.empty()) {
        if (toolchain::tar_has_exclude()) {
            for (const auto& e : o.excludes) tarcmd += " --exclude=" + sh_quote(e);
        } else {
            log_warn("[tar] 当前 tar 不支持 --exclude，恢复时会保留 cache/lib 等目录");
        }
    }
    if (o.recursive_unlink) {
        if (toolchain::tar_has_recursive_unlink()) {
            tarcmd += " --recursive-unlink";
        } else {
            log_warn("[tar] 当前 tar 不支持 --recursive-unlink，改为覆盖写入");
        }
    }

    // -xmpf：extract + 不恢复 mtime + preserve 权限 + file，与上游 -xmpf 一致。
    // 管道的退出码默认取最后一个命令（tar），所以 tar 失败能被正确捕获。
    std::string cmd;
    if (file_is_zstd(o.src)) {
        cmd = sh_quote(toolchain::zstd()) + " -d -c " + sh_quote(o.src) + " | ";
        cmd += tarcmd + " -xmpf - -C " + sh_quote(o.dstDir);
    } else {
        cmd = tarcmd + " -xmpf " + sh_quote(o.src) + " -C " + sh_quote(o.dstDir);
    }

    ExecResult r = run_shell(cmd);
    if (r.code != 0) {
        log_error("[tar] 解包失败 (code=%d): %s", r.code, trim(r.out).c_str());
        return false;
    }
    return true;
}

}  // namespace appbackup
