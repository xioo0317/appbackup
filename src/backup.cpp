#include "backup.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

#include "common.h"
#include "dbformat.h"
#include "fingerprint.h"
#include "fsops.h"
#include "manifest.h"
#include "proc.h"
#include "selinux.h"
#include "sysarchive.h"
#include "uidmap.h"

namespace appbackup {

namespace {

long long epoch_millis() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

// CompressionType 的枚举名（写进 manifest 的 ct= 与 config 的 compressionType）
const char* ct_enum_name(Compression c) {
    switch (c) {
        case Compression::Zstd:
            return "ZSTD";
        case Compression::Gzip:
            return "ZSTD";
        case Compression::Plain:
        default:
            return "TAR";
    }
}

// 排除项唯一来源，逐条对齐上游 PackagesBackupUtil.backupData：
//   USER / USER_DE：<pkg>/.ota、cache、lib、code_cache、no_backup
//   DATA / OBB / MEDIA：<pkg>/cache + Backup_*
//   APK：无
// 这份列表同时用于系统 tar 的 --exclude= 与 manifest 的 excl= 字段，保证两边口径一致。
std::vector<std::string> build_exclusions(const std::string& type, const std::string& pkg) {
    std::vector<std::string> v;
    if (type == kDataTypeUser || type == kDataTypeUserDe) {
        for (const char* sub : {".ota", "cache", "lib", "code_cache", "no_backup"}) {
            v.push_back(pkg + "/" + sub);
        }
    } else if (type == kDataTypeData || type == kDataTypeObb || type == kDataTypeMedia) {
        v.push_back(pkg + "/cache");
        v.push_back("Backup_*");
    }
    return v;
}

std::vector<std::string> prune_names_for(const std::string& type) {
    if (type == kDataTypeUser || type == kDataTypeUserDe) {
        return {".ota", "cache", "lib", "code_cache", "no_backup"};
    }
    if (type == kDataTypeData || type == kDataTypeObb || type == kDataTypeMedia) {
        return {"cache"};
    }
    return {};
}

struct TypePlan {
    std::string src;                 // 实际打包的源目录
    std::string arc;                 // 归档内顶层名
    std::vector<std::string> excl;   // manifest excl= 与 tar --exclude 同源
    std::vector<std::string> prune;  // 清单指纹剪枝（常量目录名）
    int max_depth = 0;               // 0 不限；1 只看顶层（APK）
    bool flat = false;               // 顶层直接放 ./xxx（APK）
};

bool plan_for(const std::string& type, const std::string& pkg, int user, TypePlan* plan) {
    plan->excl = build_exclusions(type, pkg);
    plan->prune = prune_names_for(type);
    if (type == kDataTypeApk) {
        plan->src = package_source_dir(pkg, user);  // apk 父目录
        plan->flat = true;
        plan->max_depth = 1;
        plan->arc = "apk";
        return !plan->src.empty();
    }
    std::string tmpl = data_type_dir_template(type);
    char buf[256];
    snprintf(buf, sizeof(buf), tmpl.c_str(), user);
    plan->src = join_path(buf, pkg);
    plan->arc = pkg;
    return true;
}

void maybe_kill(const std::string& pkg, int user, KillMode mode) {
    if (mode == KillMode::None) return;
    log_info("stopping %s before backup", pkg.c_str());
    if (mode == KillMode::Am) {
        run_shell("am force-stop --user " + std::to_string(user) + " " + pkg);
    } else {
        run_shell("kill -9 $(pidof " + pkg + ") 2>/dev/null");
    }
    usleep(300 * 1000);
}

}  // namespace

int run_backup(const BackupOptions& opt) {
    if (opt.pkg.empty()) {
        log_error("backup requires --pkg");
        return 2;
    }
    if (opt.compression == Compression::Gzip) {
        log_error("DataBackup 只认 tar / tar.zst，不支持 gzip；请用 --compress zstd 或 none");
        return 2;
    }

    std::vector<std::string> types = opt.types.empty() ? all_data_types() : opt.types;
    for (const auto& t : types) {
        bool known = false;
        for (const auto& k : all_data_types()) {
            if (k == t) known = true;
        }
        if (!known) {
            log_error("unknown data type: %s", t.c_str());
            return 2;
        }
    }

    int uid = uid_for_package(opt.pkg, opt.user, 3, 300);
    if (uid < 0) {
        log_warn("cannot resolve uid for %s (user %d); backup will still proceed", opt.pkg.c_str(), opt.user);
    }

    // 先取展示信息（label/版本/时间），供目录命名 <label>_<pkg> 与 config 使用
    DbFormatInfo info;
    info.pkg = opt.pkg;
    info.user = opt.user;
    info.uid = uid;
    info.preserve_id = 0;
    info.compression_type = ct_enum_name(opt.compression);
    fill_package_display_info(opt.pkg, opt.user, &info);

    // 主备份目录固定为 user_<userId>（不带 @preserveId）。
    // 应用目录默认 <pkg>（与上游 archivesRelativeDir 一致），--label 可切成 <label>_<pkg>。
    std::string dir_label = opt.label == "auto" ? info.label : opt.label;
    std::string dest = databackup_version_dir(opt.out_dir, opt.pkg, opt.user, 0, dir_label);
    if (opt.preserve && path_exists(dest)) {
        long long pid = epoch_millis();
        std::string archived = databackup_version_dir(opt.out_dir, opt.pkg, opt.user, pid, dir_label);
        while (path_exists(archived)) {
            pid++;
            archived = databackup_version_dir(opt.out_dir, opt.pkg, opt.user, pid, dir_label);
        }
        if (rename(dest.c_str(), archived.c_str()) != 0) {
            log_error("无法归档旧主备份 %s: %s", dest.c_str(), strerror(errno));
            return 1;
        }
        log_info("旧主备份已归档为 %s", basename_of(archived).c_str());
    }
    if (!make_dirs(dest, 0755)) {
        log_error("cannot create %s", dest.c_str());
        return 1;
    }

    maybe_kill(opt.pkg, opt.user, opt.kill);

    const std::string ext = std::string(".tar") + compression_suffix(opt.compression);
    const ManifestParams mparams_base{ct_enum_name(opt.compression), opt.level, 1, false, {}};

    std::vector<DataEntry> entries;
    uint64_t total_bytes = 0;
    int failed = 0;
    int skipped_unchanged = 0;

    for (const auto& type : types) {
        DataEntry entry;
        entry.type = type;
        entry.uid = uid;

        TypePlan plan;
        bool have_src = plan_for(type, opt.pkg, opt.user, &plan);
        if (have_src && !path_exists(plan.src)) have_src = false;

        if (!have_src) {
            if (type == kDataTypeUser) {
                // 上游把 USER 缺失视为失败（应用没装好 / 数据目录没建），其余类型只算跳过
                log_error("[user] 源不存在: %s", plan.src.c_str());
                failed++;
            } else {
                log_info("[%s] 跳过（源不存在）", type.c_str());
            }
            entry.exists = false;
            entries.push_back(entry);
            continue;
        }

        entry.exists = true;
        entry.src_path = plan.src;
        entry.context = selinux_get_context(plan.src);
        entry.gid = static_cast<long>(gid_of_path(plan.src));
        entry.archive = type + ext;
        std::string dst = join_path(dest, entry.archive);

        ManifestParams params = mparams_base;
        params.excl = plan.excl;

        std::string src_hash;
        if (opt.incremental) {
            HashOptions hopt;
            hopt.max_depth = plan.max_depth;
            hopt.prune_names = plan.prune;
            src_hash = compute_source_hash(plan.src, hopt);
        }

        // 与上游一致的「源未变化则跳过」：manifest 存在、指纹相同、打包参数相同、归档还在
        bool unchanged = false;
        if (!src_hash.empty() && path_exists(dst)) {
            std::string old_hash;
            ManifestParams old_params;
            if (manifest_read(dst, &old_hash, &old_params) && old_hash == src_hash &&
                manifest_params_equal(old_params, params)) {
                unchanged = true;
            }
        }
        if (unchanged) {
            entry.bytes = file_size(dst);
            total_bytes += entry.bytes;
            skipped_unchanged++;
            if (!path_exists(dst + ".md5")) write_md5_sidecar(dst);
            log_info("[%s] 未变化，跳过重打（%s）", type.c_str(), entry.archive.c_str());
            entries.push_back(entry);
            continue;
        }

        // 直接用系统 tar + zstd 打包（与上游 TarUtil/Tar.compress 指令一致，不手写）
        SysBackupOpt sopt;
        sopt.dst = dst;
        sopt.compression = opt.compression;
        sopt.level = opt.level;
        if (type == kDataTypeApk) {
            sopt.apk_mode = true;
            sopt.srcDir = plan.src;  // apk 父目录
            sopt.src = "./*.apk";    // 只取顶层 .apk，天然不含 lib/oat 等子目录
        } else {
            sopt.srcDir = dirname_of(plan.src);
            sopt.src = plan.arc;  // 包名（归档内顶层为 <pkg>/...）
            sopt.excludes = plan.excl;
            // 上游默认不跟随符号链接：-h 仅 readFollowSymlinks 开启，这里保持默认关
        }

        uint64_t unc = 0;
        bool ok = sys_backup(sopt, &unc);
        if (!ok) {
            log_error("[%s] 归档失败: %s", type.c_str(), entry.archive.c_str());
            failed++;
            entries.push_back(entry);
            continue;
        }

        entry.bytes = file_size(dst);
        entry.raw_bytes = unc;
        total_bytes += entry.bytes;
        log_info("[%s] %s 原始 -> %s（压缩后 %s）", type.c_str(), human_size(unc).c_str(),
                 entry.archive.c_str(), human_size(entry.bytes).c_str());

        write_md5_sidecar(dst);
        if (!src_hash.empty()) manifest_write(dst, src_hash, params);
        entries.push_back(entry);
    }

    // 没有任何一种数据类型成功备份（如应用已卸载、源目录不存在）：给出明确错误，不写残缺 config
    bool any_exists = false;
    for (const auto& e : entries) {
        if (e.exists) {
            any_exists = true;
            break;
        }
    }
    if (!any_exists) {
        log_error("backup %s : 没有任何数据可备份（应用是否已卸载 / 源目录不存在？）", opt.pkg.c_str());
        return 1;
    }

    // 权限与 SSAID：上游把这两项也写进 package_restore_config.json（dataStates 里标记 Selected），
    // 恢复时先 appops reset 再逐条 grant/revoke + setMode，并按 ssaid 改 settings_ssaid.xml。
    // 我们同样采集，保证产出的备份能被上游 App 完整恢复。
    if (uid > 0) {
        fill_package_permissions(opt.pkg, opt.user, uid, &info);
        info.ssaid = read_ssaid_value(opt.user, uid);
        // 只有真的采集到内容才标记 Selected：否则上游恢复会先 appops reset 把现有权限清掉
        info.permission_selected = !info.permissions.empty();
        info.ssaid_selected = !info.ssaid.empty();
        log_info("permissions: %zu 项, ssaid: %s", info.permissions.size(),
                 info.ssaid.empty() ? "(空)" : info.ssaid.c_str());
    } else {
        info.permission_selected = false;
        info.ssaid_selected = false;
        log_warn("uid 未知，跳过权限/SSAID 采集");
    }

    if (!write_databackup_config(dest, info, entries, opt.out_dir)) failed++;

    std::string skip_note =
        skipped_unchanged > 0 ? strfmt("（%d 类未变化跳过）", skipped_unchanged) : std::string();
    log_info("backup %s -> %s : %s, uid=%d, %s%s", opt.pkg.c_str(), dest.c_str(),
             human_size(total_bytes).c_str(), uid, ct_enum_name(opt.compression), skip_note.c_str());
    return failed == 0 ? 0 : 1;
}

}  // namespace appbackup
