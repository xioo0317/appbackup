#include "restore.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>

#include "common.h"
#include "dbformat.h"
#include "fsops.h"
#include "md5.h"
#include "meta.h"
#include "proc.h"
#include "restore_policy.h"
#include "selinux.h"
#include "sink.h"
#include "tar_reader.h"
#include "uidmap.h"

namespace appbackup {

namespace {

using policy::apply_restore_exclusions;
using policy::extract_root_for;
using policy::keep_names;
using policy::target_dir;

bool is_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

std::vector<std::string> list_subdirs(const std::string& path) {
    std::vector<std::string> out;
    for (const auto& name : list_dir(path)) {
        if (is_directory(join_path(path, name))) out.push_back(name);
    }
    return out;
}

// 归档前目录下所有 *.apk（递归；DataBackup 是 flat，本工具格式在 apk/ 子目录里）
void collect_apks(const std::string& root, std::vector<std::string>* out) {
    for (const auto& name : list_dir(root)) {
        std::string p = join_path(root, name);
        if (is_directory(p)) {
            collect_apks(p, out);
        } else if (ends_with(name, ".apk")) {
            out->push_back(p);
        }
    }
}

int sdk_int() {
    static int sdk = -1;
    if (sdk < 0) {
        ExecResult r = run_shell("getprop ro.build.version.sdk");
        sdk = atoi(trim(r.out).c_str());
        if (sdk < 0) sdk = 0;
    }
    return sdk;
}

// DataBackup PmUtil 的参数差异：SDK 34+ 需要 --bypass-low-target-sdk-block，
// SDK 30+ 需要 -i com.android.vending 冒充商店安装（否则部分包被 -d 降级校验拦下）。
void append_installer_flags(std::vector<std::string>* argv) {
    int sdk = sdk_int();
    if (sdk >= 34) argv->push_back("--bypass-low-target-sdk-block");
    if (sdk >= 30) {
        argv->push_back("-i");
        argv->push_back("com.android.vending");
    }
}

std::string parse_session_id(const std::string& out) {
    size_t lb = out.find('[');
    if (lb != std::string::npos) {
        size_t rb = out.find(']', lb);
        if (rb != std::string::npos) {
            std::string s = out.substr(lb + 1, rb - lb - 1);
            if (is_digits(s)) return s;
        }
    }
    // 兜底：扫描最后一段连续数字
    std::string best;
    std::string cur;
    for (char c : out) {
        if (c >= '0' && c <= '9') {
            cur += c;
        } else {
            if (cur.size() > best.size()) best = cur;
            cur.clear();
        }
    }
    if (cur.size() > best.size()) best = cur;
    return best;
}

bool install_single_apk(const std::string& apk, int user) {
    std::vector<std::string> argv = {"pm", "install"};
    append_installer_flags(&argv);
    argv.push_back("--user");
    argv.push_back(std::to_string(user));
    for (const char* f : {"-r", "-t", "-d"}) argv.push_back(f);
    argv.push_back(apk);

    ExecResult r = run_exec(argv);
    if (r.code != 0) {
        std::vector<std::string> alt = {"cmd", "package", "install"};
        append_installer_flags(&alt);
        alt.push_back("--user");
        alt.push_back(std::to_string(user));
        for (const char* f : {"-r", "-t", "-d"}) alt.push_back(f);
        alt.push_back(apk);
        r = run_exec(alt);
    }
    if (r.code != 0) {
        log_warn("pm install failed: %s", trim(r.out).c_str());
        return false;
    }
    return true;
}

// 多 APK（base + split）：必须走 install-create / install-write / install-commit，
// 逐个 pm install 会把 split 当成独立包而失败。
bool install_split_apks(const std::vector<std::string>& apks, int user) {
    std::vector<std::string> create = {"pm", "install-create"};
    append_installer_flags(&create);
    create.push_back("--user");
    create.push_back(std::to_string(user));
    create.push_back("-t");
    create.push_back("-d");

    ExecResult r = run_exec(create);
    std::string session = r.code == 0 ? parse_session_id(r.out) : std::string();
    if (session.empty()) {
        log_error("install-create failed: %s", trim(r.out).c_str());
        return false;
    }
    log_info("install session: %s", session.c_str());

    bool ok = true;
    for (const auto& apk : apks) {
        ExecResult w = run_exec({"pm", "install-write", session, basename_of(apk), apk});
        if (w.code != 0) {
            log_warn("install-write %s failed: %s", basename_of(apk).c_str(), trim(w.out).c_str());
            ok = false;
        }
    }
    ExecResult c = run_exec({"pm", "install-commit", session});
    if (c.code != 0) {
        log_warn("install-commit failed: %s", trim(c.out).c_str());
        ok = false;
    }
    return ok;
}

bool install_apk_from(const std::string& in_dir, const BackupMeta& meta, int user) {
    bool any = false;
    for (const auto& e : meta.entries) {
        if (e.type != kDataTypeApk || !e.exists || e.archive.empty()) continue;
        std::string archive = join_path(in_dir, e.archive);
        if (!path_exists(archive)) continue;

        std::string tmp = strfmt("/data/local/tmp/appbackup_apk_%d", static_cast<int>(getpid()));
        run_shell("rm -rf '" + tmp + "'");
        make_dirs(tmp, 0755);

        ExtractOptions eopt;
        eopt.dest_dir = tmp;
        eopt.restore_owner = false;
        eopt.preserve_mtime = false;
        std::unique_ptr<Source> src = open_source_auto(archive);
        if (!src) {
            log_error("cannot open apk archive: %s", archive.c_str());
            continue;
        }
        ExtractStats st;
        tar_extract(*src, eopt, &st);
        src->close();

        std::vector<std::string> apks;
        collect_apks(tmp, &apks);
        if (apks.empty()) {
            log_error("no apk found in %s", archive.c_str());
            run_shell("rm -rf '" + tmp + "'");
            continue;
        }
        // base.apk 排最前，其余按名字稳定排序
        std::sort(apks.begin(), apks.end());
        for (auto it = apks.begin(); it != apks.end(); ++it) {
            if (basename_of(*it) == "base.apk") {
                std::iter_swap(apks.begin(), it);
                break;
            }
        }

        log_info("installing %zu apk(s) from %s", apks.size(), e.archive.c_str());
        bool ok = apks.size() == 1 ? install_single_apk(apks[0], user) : install_split_apks(apks, user);
        if (ok) {
            any = true;
        } else {
            log_error("install failed for %s", e.archive.c_str());
        }
        run_shell("rm -rf '" + tmp + "'");
    }
    if (any) usleep(500 * 1000);
    return any;
}

// 读取备份目录：优先 meta.json（本工具格式），否则按 DataBackup 结构解析
bool load_backup(const std::string& in_dir, BackupMeta* meta) {
    std::string meta_path = join_path(in_dir, "meta.json");
    if (path_exists(meta_path)) {
        return read_meta_file(meta_path, meta);
    }

    if (!is_databackup_dir(in_dir)) {
        log_error("%s 里既没有 meta.json，也不是 DataBackup 备份目录", in_dir.c_str());
        return false;
    }

    DbFormatInfo info;
    if (!read_databackup_info(in_dir, &info)) {
        log_error("无法解析 DataBackup 备份目录: %s", in_dir.c_str());
        return false;
    }

    meta->tool = "DataBackup";
    meta->tool_version = "compatible";
    meta->pkg = info.pkg;
    meta->user = info.user;
    meta->uid = info.uid;
    meta->gid = info.uid;
    meta->created_at = now_iso8601();
    meta->compression = info.compression_type.empty() ? "zstd" : info.compression_type;
    log_info("识别为 DataBackup 备份：pkg=%s user=%d preserveId=%lld ct=%s", info.pkg.c_str(), info.user,
             info.preserve_id, info.compression_type.c_str());

    for (const auto& type : all_data_types()) {
        DataEntry e;
        e.type = type;
        e.arc_name = type == kDataTypeApk ? "apk" : info.pkg;
        std::string archive = find_databackup_archive(in_dir, type);
        e.exists = !archive.empty();
        if (e.exists) {
            e.archive = basename_of(archive);
            e.bytes = file_size(archive);
        }
        meta->entries.push_back(e);
    }
    return true;
}

// 备份目录自动下探：支持传备份根目录（…/DataBackup）、应用目录（…/apps/<pkg>）或版本目录。
// DataBackup 真实布局：<root>/apps/<label>_<pkg>/user_<uid>@<preserveId>，旧版是 <root>/apps/<pkg>/user_<uid>。
std::string resolve_backup_dir(const std::string& in, const std::string& pkg_hint) {
    if (path_exists(join_path(in, "meta.json")) || path_exists(join_path(in, "package_restore_config.json"))) {
        return in;
    }
    // 版本目录（含无 json 的老备份）：user_<uid>[@preserveId]
    if (starts_with(basename_of(in), "user_")) return in;
    // 应用目录：直接含 user_* 子目录
    std::vector<std::string> sub = list_subdirs(in);

    // 自有格式备份根目录：<in>/<pkg>_<YYYYmmdd_HHMMSS>/meta.json。
    // 包名允许含下划线，所以包名只信 meta.json 里的字段，不从目录名反推。
    {
        struct OwnCand {
            std::string path;
            std::string pkg;
            long long mtime = 0;
        };
        std::vector<OwnCand> own;
        for (const auto& s : sub) {
            std::string p = join_path(in, s);
            std::string mj = join_path(p, "meta.json");
            if (!path_exists(mj)) continue;
            BackupMeta m;
            if (!read_meta_file(mj, &m)) continue;
            if (!pkg_hint.empty() && m.pkg != pkg_hint) continue;
            struct stat st;
            OwnCand c;
            c.path = p;
            c.pkg = m.pkg;
            c.mtime = stat(p.c_str(), &st) == 0 ? static_cast<long long>(st.st_mtime) : 0;
            own.push_back(c);
        }
        if (!own.empty()) {
            if (pkg_hint.empty()) {
                std::vector<std::string> pkgs;
                for (const auto& c : own) {
                    if (std::find(pkgs.begin(), pkgs.end(), c.pkg) == pkgs.end()) pkgs.push_back(c.pkg);
                }
                if (pkgs.size() > 1) {
                    log_error("%s 下有 %zu 个应用备份，请用 --pkg 指定：", in.c_str(), pkgs.size());
                    for (const auto& p : pkgs) log_error("  %s", p.c_str());
                    return std::string();
                }
            }
            const OwnCand* best = &own[0];
            for (const auto& c : own) {
                if (c.mtime > best->mtime) best = &c;
            }
            log_info("自动定位备份目录: %s", best->path.c_str());
            return best->path;
        }
    }

    bool has_version = false;
    for (const auto& s : sub) {
        if (starts_with(s, "user_")) has_version = true;
    }

    std::string apps_dir;
    std::string app_scope;
    if (has_version) {
        apps_dir = dirname_of(in);
        app_scope = in;
    } else if (path_exists(join_path(in, "apps"))) {
        apps_dir = join_path(in, "apps");
    } else {
        return in;  // 交给 load_backup 报错
    }

    struct Cand {
        std::string path;
        std::string app_dir;
        std::string pkg;
        long long preserve = 0;
        long long mtime = 0;
    };
    std::vector<Cand> cands;
    std::vector<std::string> app_dirs;
    if (!app_scope.empty()) {
        app_dirs.push_back(basename_of(app_scope));
    } else {
        app_dirs = list_subdirs(apps_dir);
    }
    for (const auto& app : app_dirs) {
        std::string app_path = app_scope.empty() ? join_path(apps_dir, app) : app_scope;
        for (const auto& v : list_subdirs(app_path)) {
            if (!starts_with(v, "user_")) continue;
            Cand c;
            c.path = join_path(app_path, v);
            c.app_dir = app;
            DbFormatInfo info;
            c.pkg = read_databackup_info(c.path, &info) ? info.pkg : app;
            c.preserve = info.preserve_id;
            struct stat st;
            c.mtime = stat(c.path.c_str(), &st) == 0 ? static_cast<long long>(st.st_mtime) : 0;
            if (!pkg_hint.empty() && c.pkg != pkg_hint && app != pkg_hint && !ends_with(app, "_" + pkg_hint)) {
                continue;
            }
            cands.push_back(c);
        }
    }
    if (cands.empty()) return in;

    // 未指定包名且候选跨多个包：拒绝猜，避免恢复到错误应用
    if (pkg_hint.empty()) {
        std::vector<std::string> pkgs;
        for (const auto& c : cands) {
            if (std::find(pkgs.begin(), pkgs.end(), c.pkg) == pkgs.end()) pkgs.push_back(c.pkg);
        }
        if (pkgs.size() > 1) {
            log_error("%s 下有 %zu 个应用备份，请用 --pkg 指定：", in.c_str(), pkgs.size());
            for (const auto& p : pkgs) log_error("  %s", p.c_str());
            return std::string();
        }
    }

    const Cand* best = &cands[0];
    for (const auto& c : cands) {
        if (c.preserve != best->preserve ? c.preserve > best->preserve : c.mtime > best->mtime) best = &c;
    }
    log_info("自动定位备份目录: %s", best->path.c_str());
    return best->path;
}

struct ChecksumResult {
    bool present = false;
    bool mismatch = false;
    std::string expected;
    std::string actual;
};

// 与 DataBackup ChecksumUtil.verify 语义一致：
//   无 .md5 / 内容为空 / 算不出 MD5 → 视为无需校验（放行）；算得出且不一致才算 mismatch。
ChecksumResult verify_checksum(const std::string& archive) {
    ChecksumResult r;
    std::string md5_path = archive + ".md5";
    if (!path_exists(md5_path)) return r;
    std::string expected = trim(read_file_head(md5_path, 4096));
    if (expected.empty()) return r;
    r.present = true;
    r.expected = expected;
    r.actual = md5_of_file(archive);
    if (r.actual.empty()) return r;
    std::string a = expected;
    std::string b = r.actual;
    for (auto& c : a) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (auto& c : b) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    r.mismatch = a != b;
    return r;
}

}  // namespace

int run_restore(const RestoreOptions& opt) {
    if (opt.in_dir.empty()) {
        log_error("restore requires --in <备份目录>");
        return 2;
    }

    std::string in_dir = resolve_backup_dir(opt.in_dir, opt.pkg);
    if (in_dir.empty()) return 2;

    BackupMeta meta;
    if (!load_backup(in_dir, &meta)) return 1;

    std::string pkg = opt.pkg.empty() ? meta.pkg : opt.pkg;
    if (pkg.empty()) {
        log_error("no package name in backup metadata, use --pkg");
        return 2;
    }
    int user = opt.user;
    if (user == 0 && meta.user != 0) user = meta.user;

    log_info("restore %s (user %d) from %s", pkg.c_str(), user, in_dir.c_str());

    // 顺序与 DataBackup AbstractRestoreService 一致：先停进程（避免旧进程持有文件/dex），
    // 再装 APK（installd 会按本机 uid 创建数据目录并打好 SELinux 标签），最后写数据。
    if (opt.force_stop) run_shell("am force-stop --user " + std::to_string(user) + " " + pkg);

    if (opt.install_apk) install_apk_from(in_dir, meta, user);

    int uid = uid_for_package(pkg, user, 20, 500);
    if (uid < 0) {
        log_error("cannot resolve uid for %s on this device; is the app installed for user %d?", pkg.c_str(), user);
        return 1;
    }
    log_info("target uid=%d", uid);

    std::vector<std::string> want = opt.types;
    int failed = 0;

    for (const auto& e : meta.entries) {
        if (!e.exists) continue;
        if (!want.empty()) {
            bool hit = false;
            for (const auto& t : want) {
                if (t == e.type) hit = true;
            }
            if (!hit) continue;
        }
        if (e.type == kDataTypeApk) continue;  // APK 已单独安装

        std::string dest = target_dir(e.type, user, pkg);
        if (dest.empty()) {
            log_warn("[%s] unknown target dir, skip", e.type.c_str());
            continue;
        }
        std::string archive = join_path(in_dir, e.archive);
        if (!path_exists(archive)) {
            log_warn("[%s] archive missing: %s", e.type.c_str(), archive.c_str());
            failed++;
            continue;
        }

        // 1) 完整性校验（DataBackup 的 <归档>.md5）
        if (opt.verify_checksum) {
            ChecksumResult cs = verify_checksum(archive);
            if (cs.mismatch) {
                if (!opt.force_checksum) {
                    log_error("[%s] 校验失败（MD5 不一致）: %s", e.type.c_str(), archive.c_str());
                    log_error("        expected=%s actual=%s", cs.expected.c_str(), cs.actual.c_str());
                    log_error("        如确认要强制恢复，加 --force-checksum");
                    failed++;
                    continue;
                }
                log_warn("[%s] MD5 不一致，按 --force-checksum 强制恢复", e.type.c_str());
            } else if (cs.present) {
                log_debug("[%s] md5 ok", e.type.c_str());
            }
        }

        // 2) SELinux context 必须在 mkdir 之前取：目录由 installd 按本机 uid 建好，
        //    其 MCS category 才是本机值；自己 mkdir 出来的目录 label 不可信。
        std::string target_context = selinux_get_context(dest);
        bool existed = path_exists(dest);
        if (!existed) {
            log_warn("[%s] 目标目录不存在（未装 APK？）：%s", e.type.c_str(), dest.c_str());
            make_dirs(dest, 0771);
            target_context.clear();
        }

        if (opt.clean) {
            uint64_t n = clean_dir_children(dest, keep_names());
            log_info("[%s] cleaned %llu entries under %s", e.type.c_str(), static_cast<unsigned long long>(n),
                     dest.c_str());
        }

        // 3) 解包到**父目录**：归档内是 <pkg>/...，与 DataBackup Tar.decompress(dst = dstDir) 一致。
        //    解到 dest 本身会得到 <pkg>/<pkg>/...，应用读不到数据（表现为打开闪退/数据全丢）。
        std::string parent = extract_root_for(dest);
        if (!make_dirs(parent, 0755)) {
            log_error("[%s] cannot create %s (%s)", e.type.c_str(), parent.c_str(), strerror(errno));
            failed++;
            continue;
        }

        ExtractOptions eopt;
        eopt.dest_dir = parent;
        eopt.restore_owner = true;  // 先按归档里的 uid 写入，随后统一 chown 成新机 uid
        eopt.preserve_mtime = true;
        apply_restore_exclusions(e.type, pkg, &eopt);
        std::unique_ptr<Source> src = open_source_auto(archive);
        if (!src) {
            log_error("[%s] cannot open %s", e.type.c_str(), archive.c_str());
            failed++;
            continue;
        }
        ExtractStats st;
        bool ok = tar_extract(*src, eopt, &st);
        src->close();
        if (!ok) {
            log_error("[%s] extract failed", e.type.c_str());
            failed++;
            continue;
        }
        if (!path_exists(dest)) {
            log_error("[%s] 归档内没有 %s/ 目录，解包结果不在预期位置", e.type.c_str(), pkg.c_str());
            failed++;
            continue;
        }

        // 4) 属主：外部存储的 gid 取 **父目录**（/data/media/<u>/Android/data 等，media_rw），
        //    取目标目录自身会在新建目录时拿到 0，导致应用写不进外部存储。
        gid_t gid = data_type_is_external(e.type) ? gid_of_path(parent) : static_cast<gid_t>(uid);
        if (gid == 0) gid = static_cast<gid_t>(uid);

        uint64_t chown_fail = chown_recursive(dest, static_cast<uid_t>(uid), gid);
        if (chown_fail > 0) {
            log_warn("[%s] chown %d:%d had %llu failures", e.type.c_str(), uid, static_cast<int>(gid),
                     static_cast<unsigned long long>(chown_fail));
        }

        // 5) SELinux 标签
        if (opt.do_selinux) {
            bool sel_ok = false;
            if (data_type_is_external(e.type)) {
                // 外部存储 label 由系统 file_contexts 决定，无条件 restorecon
                sel_ok = selinux_restorecon(dest, true, true);
            } else {
                if (!target_context.empty()) {
                    sel_ok = selinux_set_context(dest, target_context, true);
                }
                if (!sel_ok) {
                    std::string derived = selinux_derive_app_context(parent);
                    if (!derived.empty()) sel_ok = selinux_set_context(dest, derived, true);
                }
                if (!sel_ok) sel_ok = selinux_restorecon(dest, true, true);
            }
            if (!sel_ok) log_warn("[%s] selinux relabel failed on %s", e.type.c_str(), dest.c_str());
        }

        // 6) 属主二次校验（FUSE 上偶发 chown 未生效）
        if (opt.verify_owner) {
            uint64_t not_owned = count_not_owned_by(dest, static_cast<uid_t>(uid));
            if (not_owned > 0) {
                log_warn("[%s] %llu entries still not owned by %d, retry chown", e.type.c_str(),
                         static_cast<unsigned long long>(not_owned), uid);
                chown_recursive(dest, static_cast<uid_t>(uid), gid);
                not_owned = count_not_owned_by(dest, static_cast<uid_t>(uid));
                if (not_owned > 0) {
                    log_error("[%s] ownership still wrong for %llu entries", e.type.c_str(),
                              static_cast<unsigned long long>(not_owned));
                    failed++;
                }
            }
        }

        log_info("[%s] restored %llu files (%s) -> %s", e.type.c_str(), static_cast<unsigned long long>(st.files),
                 human_size(st.bytes).c_str(), dest.c_str());
    }

    log_info("restore finished: %s", failed == 0 ? "ok" : "with errors");
    return failed == 0 ? 0 : 1;
}

int run_list(const std::string& in_dir_raw, const std::string& pkg_hint) {
    std::string in_dir = resolve_backup_dir(in_dir_raw, pkg_hint);
    if (in_dir.empty()) return 2;
    BackupMeta meta;
    if (!load_backup(in_dir, &meta)) return 1;
    printf("%-8s %-6s %-10s %-12s %-8s %-30s\n", "TYPE", "EXISTS", "FILES", "SIZE", "MD5", "ARCHIVE");
    for (const auto& e : meta.entries) {
        const char* md5 = "-";
        if (e.exists && !e.archive.empty()) {
            ChecksumResult cs = verify_checksum(join_path(in_dir, e.archive));
            md5 = !cs.present ? "none" : (cs.mismatch ? "BAD" : "ok");
        }
        printf("%-8s %-6s %-10llu %-12s %-8s %-30s\n", e.type.c_str(), e.exists ? "yes" : "no",
               static_cast<unsigned long long>(e.files), human_size(e.bytes).c_str(), md5, e.archive.c_str());
    }
    printf("\npkg=%s user=%d uid=%d compression=%s created=%s tool=%s\n", meta.pkg.c_str(), meta.user, meta.uid,
           meta.compression.c_str(), meta.created_at.c_str(), meta.tool.c_str());
    return 0;
}

int run_info(const std::string& pkg, int user) {
    int uid = uid_for_package(pkg, user, 1, 0);
    if (uid < 0) {
        printf("package: %s\ninstalled: no (user %d)\n", pkg.c_str(), user);
        return 1;
    }
    printf("package: %s\nuser: %d\nuid: %d\n", pkg.c_str(), user, uid);
    for (const auto& t : all_data_types()) {
        std::string path;
        if (t == kDataTypeApk) {
            path = package_source_dir(pkg, user);
        } else {
            std::string tmpl = data_type_dir_template(t);
            char buf[256];
            snprintf(buf, sizeof(buf), tmpl.c_str(), user);
            path = join_path(buf, pkg);
        }
        if (path.empty() || !path_exists(path)) {
            printf("%-8s -  (missing)\n", t.c_str());
            continue;
        }
        printf("%-8s %-56s gid=%-6d ctx=%s\n", t.c_str(), path.c_str(), static_cast<int>(gid_of_path(path)),
               selinux_get_context(path).c_str());
    }
    return 0;
}

}  // namespace appbackup
