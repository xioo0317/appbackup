#include "sysarchive.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common.h"
#include "fsops.h"
#include "proc.h"
#include "sink.h"

namespace appbackup {

namespace {

// 单引号转义：' -> '\''
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

bool g_zstd_priority_checked = false;
bool g_zstd_priority_ok = false;

// 上游用 --priority=rt（实时调度）；部分 zstd 构建不支持该选项，探测一次后缓存。
bool zstd_supports_priority_rt() {
    if (g_zstd_priority_checked) return g_zstd_priority_ok;
    g_zstd_priority_checked = true;
    ExecResult r = run_shell("zstd --help 2>&1");
    g_zstd_priority_ok = (r.code == 0 && r.out.find("priority") != std::string::npos);
    return g_zstd_priority_ok;
}

// 上游压缩参数：zstd -r -T0 --ultra -q [--priority=rt] -<level>
std::string zstd_compress_cmd(int level, bool with_priority) {
    std::string c = "zstd -r -T0 --ultra -q";
    if (with_priority && zstd_supports_priority_rt()) c += " --priority=rt";
    c += " -" + std::to_string(level);
    return c;
}

}  // namespace

bool sys_backup(const SysBackupOpt& o, uint64_t* uncompressed_bytes) {
    if (uncompressed_bytes) *uncompressed_bytes = 0;

    // 临时 tar 落盘，再交给 zstd（与管道输出字节完全一致，退出码干净）。
    std::string tmp = o.dst + ".tar.tmp";
    unlink(tmp.c_str());

    std::string tar_cmd;
    if (o.apk_mode) {
        // 上游：cd srcDir && tar --totals -cpf tmp ./*.apk ; cd /
        // 用 ./*.apk glob，所以只会打包顶层 apk，天然不含 lib/oat 等子目录。
        tar_cmd = "cd " + sh_quote(o.srcDir) + " && { tar --totals -cpf " + sh_quote(tmp) + " " + o.src +
                  " ; } ; cd /";
    } else {
        tar_cmd = "tar --totals";
        for (const auto& e : o.excludes) tar_cmd += " --exclude=" + sh_quote(e);
        if (o.follow_symlinks) tar_cmd += " -h";
        tar_cmd += " -cpf " + sh_quote(tmp) + " -C " + sh_quote(o.srcDir) + " -- " + sh_quote(o.src);
    }

    ExecResult r = run_shell(tar_cmd);
    if (r.code != 0) {
        log_error("[tar] 打包失败 (code=%d): %s", r.code, trim(r.out).c_str());
        unlink(tmp.c_str());
        return false;
    }
    if (!path_exists(tmp)) {
        log_error("[tar] 未产出临时归档: %s", tmp.c_str());
        unlink(tmp.c_str());
        return false;
    }
    if (uncompressed_bytes) *uncompressed_bytes = static_cast<uint64_t>(file_size(tmp));

    bool ok = false;
    if (o.compression == Compression::Zstd) {
        std::string zcmd = zstd_compress_cmd(o.level, true) + " -o " + sh_quote(o.dst) + " " + sh_quote(tmp);
        ExecResult zr = run_shell(zcmd);
        if (zr.code != 0 && zstd_supports_priority_rt()) {
            // 重试去掉 --priority=rt（部分 zstd 不支持）
            std::string zcmd2 =
                zstd_compress_cmd(o.level, false) + " -o " + sh_quote(o.dst) + " " + sh_quote(tmp);
            zr = run_shell(zcmd2);
        }
        ok = (zr.code == 0);
        if (!ok) log_error("[zstd] 压缩失败 (code=%d): %s", zr.code, trim(zr.out).c_str());
    } else if (o.compression == Compression::Gzip) {
        ExecResult zr =
            run_shell("gzip -" + std::to_string(o.level) + " -c " + sh_quote(tmp) + " > " + sh_quote(o.dst));
        ok = (zr.code == 0);
        if (!ok) log_error("[gzip] 压缩失败 (code=%d): %s", zr.code, trim(zr.out).c_str());
    } else {
        // 不压缩：直接重命名
        ok = (rename(tmp.c_str(), o.dst.c_str()) == 0);
        if (!ok) log_error("[tar] rename 失败: %s", strerror(errno));
    }

    unlink(tmp.c_str());
    return ok && path_exists(o.dst);
}

bool sys_restore(const SysRestoreOpt& o) {
    // 先解压（如需）到临时 tar，再交给 tar 解包；与上游 zstd -d -c | tar 字节一致。
    std::string tmp;
    bool need_unlink = false;
    if (file_is_zstd(o.src)) {
        tmp = o.src + ".untar.tmp";
        unlink(tmp.c_str());
        ExecResult d = run_shell("zstd -d -c " + sh_quote(o.src) + " -o " + sh_quote(tmp));
        if (d.code != 0) {
            log_error("[zstd] 解压失败 (code=%d): %s", d.code, trim(d.out).c_str());
            unlink(tmp.c_str());
            return false;
        }
        need_unlink = true;
    } else {
        tmp = o.src;  // 普通 tar，直接当源
    }

    // 目标目录必须存在（tar -C 不会自建）；上游恢复前也会先 mkdirs
    if (!path_exists(o.dstDir) && !make_dirs(o.dstDir, 0755)) {
        log_error("[tar] 无法创建目标目录: %s (%s)", o.dstDir.c_str(), strerror(errno));
        if (need_unlink) unlink(tmp.c_str());
        return false;
    }

    std::string tar_cmd = "tar --totals";
    for (const auto& e : o.excludes) tar_cmd += " --exclude=" + sh_quote(e);
    if (o.recursive_unlink) tar_cmd += " --recursive-unlink";
    // -xmpf：extract + touch(不恢复 mtime) + preserve 权限 + file；与上游 -xmpf 对齐。
    tar_cmd += " -xmpf " + sh_quote(tmp) + " -C " + sh_quote(o.dstDir);

    ExecResult r = run_shell(tar_cmd);
    if (need_unlink) unlink(tmp.c_str());
    if (r.code != 0) {
        log_error("[tar] 解包失败 (code=%d): %s", r.code, trim(r.out).c_str());
        return false;
    }
    return true;
}

}  // namespace appbackup
