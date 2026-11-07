#include "backup.h"

#include <dirent.h>
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

// 排除项唯一来源，逐条对齐上游 PackagesBackupUtil.backupData + 实际样本：
//   USER / USER_DE：<pkg>/.ota、cache、lib、code_cache、no_backup
//   DATA / OBB / MEDIA：<pkg>/cache、Backup_*、.trashed-*、.pending-*、.Trashed-*、.Pending-*
//   APK：无
// 后四项是外部存储的回收站/待安装临时文件命名，在开源 main 分支的 backupData 里没有，
// 但两个独立样本和用户模板的 excl= 都稳定出现，属于该版本默认（或设置里追加）的规则。
// 这份列表同时用于系统 tar 的 --exclude= 与 manifest 的 excl= 字段，保证两边口径一致。
std::vector<std::string> build_exclusions(const std::string& type, const std::string& pkg) {
    std::vector<std::string> v;
    if (type == kDataTypeUser || type == kDataTypeUserDe) {
        for (const char* sub : {".ota", "cache", "lib", "code_cache", "no_backup"}) {
            v.push_back(pkg + "/" + sub);
        }
    } else if (type == kDataTypeData || type == kDataTypeObb || type == kDataTypeMedia) {
        v.push_back(pkg + "/cache");
        for (const char* pat : {"Backup_*", ".trashed-*", ".pending-*", ".Trashed-*", ".Pending-*"}) {
            v.push_back(pat);
        }
    }
    return v;
}

// 源目录里所有普通文件的大小总和（不跟随符号链接、不剪枝）。
// 上游 dataStats 取的是 calculateSize(src)：打包前源目录的原始字节数，
// 包含随后被 --exclude 排除的 cache/lib 等，所以这里不能按 excl 剪枝。
uint64_t dir_total_bytes(const std::string& root) {
    uint64_t total = 0;
    std::vector<std::string> stack{root};
    while (!stack.empty()) {
        std::string d = stack.back();
        stack.pop_back();
        DIR* dp = opendir(d.c_str());
        if (!dp) continue;
        struct dirent* de;
        while ((de = readdir(dp)) != nullptr) {
            std::string name = de->d_name;
            if (name == "." || name == "..") continue;
            std::string p = d + "/" + name;
            struct stat st;
            if (lstat(p.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                stack.push_back(p);
            } else if (S_ISREG(st.st_mode)) {
                total += static_cast<uint64_t>(st.st_size);
            }
        }
        closedir(dp);
    }
    return total;
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
        log_error("DataBackup accepts tar / tar.zst only, gzip unsupported; use --compress zstd or none");
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
    // 应用目录默认 <label>_<pkg>（与上游 App 产物一致）；--label plain 可退回纯 <pkg>。
    std::string dir_label;
    if (opt.label == "plain") {
        dir_label.clear();
    } else if (opt.label == "auto") {
        dir_label = info.label;
        if (dir_label.empty()) {
            log_warn("no app name, fallback apps/<pkg>; --label <label>");
        }
    } else {
        dir_label = opt.label;
    }
    // 上游 config 的 packageInfo.label 与目录前缀同源；显式 --label / 采集成功时都写进 config。
    if (!dir_label.empty() && info.label.empty()) info.label = dir_label;
    std::string dest = databackup_version_dir(opt.out_dir, opt.pkg, opt.user, 0, dir_label);
    if (opt.preserve && path_exists(dest)) {
        long long pid = epoch_millis();
        std::string archived = databackup_version_dir(opt.out_dir, opt.pkg, opt.user, pid, dir_label);
        while (path_exists(archived)) {
            pid++;
            archived = databackup_version_dir(opt.out_dir, opt.pkg, opt.user, pid, dir_label);
        }
        if (rename(dest.c_str(), archived.c_str()) != 0) {
            log_error("cannot archive previous backup %s: %s", dest.c_str(), strerror(errno));
            return 1;
        }
        log_info("previous backup archived as %s", basename_of(archived).c_str());
    }
    if (!make_dirs(dest, 0755)) {
        log_error("cannot create %s", dest.c_str());
        return 1;
    }

    maybe_kill(opt.pkg, opt.user, opt.kill);

    const std::string ext = std::string(".tar") + compression_suffix(opt.compression);
    const ManifestParams mparams_base{ct_enum_name(opt.compression), opt.level, opt.threads, false, {}};

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
                log_error("[user] src missing: %s", plan.src.c_str());
                failed++;
            } else {
                log_info("[%s] skip", type.c_str());
            }
            entry.exists = false;
            entries.push_back(entry);
            continue;
        }

        entry.exists = true;
        entry.src_path = plan.src;
        // config.dataStats：打包前源目录原始大小。USER 例外——上游刻意跳过 calculateSize
        // （海量小文件遍历要几十秒且发热），用归档大小回填，见 write_databackup_config。
        if (type != kDataTypeUser) entry.src_bytes = dir_total_bytes(plan.src);
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
            log_info("[%s] unchanged, skipped (%s)", type.c_str(), entry.archive.c_str());
            entries.push_back(entry);
            continue;
        }

        // 用 tar + zstd 打包（指令与上游 TarUtil 一致，不手写）
        SysBackupOpt sopt;
        sopt.dst = dst;
        sopt.compression = opt.compression;
        sopt.level = opt.level;
        sopt.threads = opt.threads;
        bool skipped_no_apk = false;
        if (type == kDataTypeApk) {
            sopt.apk_mode = true;
            sopt.srcDir = plan.src;  // apk 所在目录
            sopt.skipped = &skipped_no_apk;
        } else {
            sopt.srcDir = dirname_of(plan.src);
            sopt.src = plan.arc;  // 包名（归档内顶层为 <pkg>/...）
            sopt.excludes = plan.excl;
            // 上游默认不跟随符号链接：-h 仅 readFollowSymlinks 开启，这里保持默认关
        }

        uint64_t unc = 0;
        bool ok = sys_backup(sopt, &unc);
        if (skipped_no_apk) {
            // 目录里没有 .apk（应用已卸载 / apk 被清）：与其他缺失类型一致，不算失败
            entry.exists = false;
            entries.push_back(entry);
            continue;
        }
        if (!ok) {
            log_error("[%s] archive failed: %s", type.c_str(), entry.archive.c_str());
            entry.exists = false;
            entries.push_back(entry);
            failed++;
            continue;
        }

        entry.bytes = file_size(dst);
        entry.raw_bytes = unc > 0 ? unc : entry.bytes;
        total_bytes += entry.bytes;
        log_info("[%s] %s raw -> %s tarball %s", type.c_str(), human_size(unc).c_str(),
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
        log_error("backup %s: nothing to back up (app uninstalled / src missing?)", opt.pkg.c_str());
        return 1;
    }

    // 权限与 SSAID：上游 PackageDataStates 默认 8 项全 Selected，恢复时按状态执行
    // appops reset → 逐条 grant/revoke + setMode，以及写 settings_ssaid.xml。
    // 这里默认也写 Selected（与模板一致）；只有在采集不到内容时才退回 Disabled，
    // 避免上游先做 appops reset 却没有任何权限可回填，把设备现有权限清空。
    if (uid > 0) {
        fill_package_permissions(opt.pkg, opt.user, uid, &info);
        info.ssaid = read_ssaid_value(opt.user, uid);
        info.permission_selected = opt.backup_permissions && !info.permissions.empty();
        info.ssaid_selected = opt.backup_ssaid && !info.ssaid.empty();
        log_info("permissions: %zu items, ssaid: %s", info.permissions.size(),
                 info.ssaid.empty() ? "(empty)" : info.ssaid.c_str());
    } else {
        info.permission_selected = false;
        info.ssaid_selected = false;
        log_warn("uid unknown, skipping permissions/SSAID");
    }
    if (opt.backup_permissions && !info.permission_selected) {
        log_warn("no permissions collected, permissionState=Disabled (avoid wiping device perms on restore)");
    }
    if (opt.backup_ssaid && !info.ssaid_selected) {
        log_warn("empty SSAID, ssaidState=Disabled");
    }

    if (!write_databackup_config(dest, info, entries, opt.out_dir)) failed++;

    std::string skip_note =
        skipped_unchanged > 0 ? strfmt(" (%d unchanged, skipped)", skipped_unchanged) : std::string();
    log_info("backup %s -> %s : %s, uid=%d, %s%s", opt.pkg.c_str(), dest.c_str(),
             human_size(total_bytes).c_str(), uid, ct_enum_name(opt.compression), skip_note.c_str());
    return failed == 0 ? 0 : 1;
}

}  // namespace appbackup
