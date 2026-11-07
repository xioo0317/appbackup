#include "restore.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>

#include "common.h"
#include "fsops.h"
#include "meta.h"
#include "proc.h"
#include "selinux.h"
#include "sink.h"
#include "tar_reader.h"
#include "uidmap.h"

namespace ozj {

namespace {

// 恢复时保留的目录名（与 DataBackup cleanRestore 一致）
std::vector<std::string> keep_names() { return {"lib", "cache", "code_cache", "no_backup", ".ota"}; }

// 解包阶段的排除项：不覆盖目标机器已有的缓存/库，避免 ABI 与 dalvik 缓存不匹配导致闪退
void fill_restore_exclusions(const std::string& type, const std::string& pkg, ExtractOptions* opt) {
    if (type == kDataTypeUser || type == kDataTypeUserDe) {
        for (const char* sub : {"cache", "code_cache", "no_backup"}) {
            opt->exclude_prefixes.push_back(pkg + "/" + sub);
        }
    } else if (type == kDataTypeData || type == kDataTypeObb || type == kDataTypeMedia) {
        opt->exclude_prefixes.push_back(pkg + "/cache");
    }
}

std::string target_dir_for(const DataEntry& e, int user, const std::string& pkg) {
    if (e.type == kDataTypeApk) return std::string();
    std::string tmpl = data_type_dir_template(e.type);
    if (tmpl.empty()) return std::string();
    char buf[256];
    snprintf(buf, sizeof(buf), tmpl.c_str(), user);
    return join_path(buf, pkg);
}

bool install_apk_from(const std::string& in_dir, const BackupMeta& meta, int user) {
    bool any = false;
    for (const auto& e : meta.entries) {
        if (e.type != kDataTypeApk || !e.exists || e.archive.empty()) continue;
        std::string archive = join_path(in_dir, e.archive);
        if (!path_exists(archive)) continue;
        std::string tmp = strfmt("/data/local/tmp/ozj_apk_%d", static_cast<int>(getpid()));
        make_dirs(tmp, 0755);
        ExtractOptions eopt;
        eopt.dest_dir = tmp;
        eopt.restore_owner = false;
        eopt.preserve_mtime = false;
        std::unique_ptr<Source> src = file_is_gzip(archive) ? open_gzip_source(archive) : open_plain_source(archive);
        if (!src) {
            log_error("cannot open apk archive: %s", archive.c_str());
            continue;
        }
        ExtractStats st;
        tar_extract(*src, eopt, &st);
        src->close();
        for (const auto& name : list_dir(tmp)) {
            if (!ends_with(name, ".apk")) continue;
            std::string apk = join_path(tmp, name);
            log_info("installing %s", apk.c_str());
            ExecResult r = run_exec({"pm", "install", "-r", "-d", "--user", std::to_string(user), apk});
            if (r.code != 0) {
                r = run_exec({"cmd", "package", "install", "-r", "-d", "--user", std::to_string(user), apk});
            }
            if (r.code != 0) {
                log_warn("pm install failed: %s", trim(r.out).c_str());
            } else {
                any = true;
            }
        }
        run_shell("rm -rf '" + tmp + "'");
    }
    if (any) usleep(500 * 1000);
    return any;
}

}  // namespace

int run_restore(const RestoreOptions& opt) {
    if (opt.in_dir.empty()) {
        log_error("restore requires --in <备份目录>");
        return 2;
    }
    std::string meta_path = join_path(opt.in_dir, "meta.json");
    BackupMeta meta;
    if (!read_meta_file(meta_path, &meta)) {
        log_error("cannot read %s", meta_path.c_str());
        return 1;
    }

    std::string pkg = opt.pkg.empty() ? meta.pkg : opt.pkg;
    if (pkg.empty()) {
        log_error("no package name in meta.json, use --pkg");
        return 2;
    }
    int user = opt.user;
    if (user == 0 && meta.user != 0) user = meta.user;

    log_info("restore %s (user %d) from %s", pkg.c_str(), user, opt.in_dir.c_str());

    if (opt.install_apk) {
        install_apk_from(opt.in_dir, meta, user);
    }

    if (opt.force_stop) {
        run_shell("am force-stop --user " + std::to_string(user) + " " + pkg);
    }

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

        std::string dest = target_dir_for(e, user, pkg);
        if (dest.empty()) {
            log_warn("[%s] unknown target dir, skip", e.type.c_str());
            continue;
        }
        std::string archive = join_path(opt.in_dir, e.archive);
        if (!path_exists(archive)) {
            log_warn("[%s] archive missing: %s", e.type.c_str(), archive.c_str());
            failed++;
            continue;
        }

        if (!make_dirs(dest, 0771)) {
            log_error("[%s] cannot create %s (%s)", e.type.c_str(), dest.c_str(), strerror(errno));
            failed++;
            continue;
        }

        if (opt.clean) {
            uint64_t n = clean_dir_children(dest, keep_names());
            log_info("[%s] cleaned %llu entries under %s", e.type.c_str(), static_cast<unsigned long long>(n),
                     dest.c_str());
        }

        ExtractOptions eopt;
        eopt.dest_dir = dest;
        eopt.restore_owner = true;  // 先按归档里的 uid 写入，随后统一 chown 成新机 uid
        eopt.preserve_mtime = true;
        fill_restore_exclusions(e.type, pkg, &eopt);

        std::unique_ptr<Source> src = file_is_gzip(archive) ? open_gzip_source(archive) : open_plain_source(archive);
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

        // gid：外部存储沿用目标目录现有 gid（media_rw / FUSE），其余与 uid 一致
        gid_t gid = data_type_is_external(e.type) ? gid_of_path(dest) : static_cast<gid_t>(uid);
        if (gid == 0) gid = static_cast<gid_t>(uid);

        uint64_t chown_fail = chown_recursive(dest, static_cast<uid_t>(uid), gid);
        if (chown_fail > 0) {
            log_warn("[%s] chown %d:%d had %llu failures", e.type.c_str(), uid, static_cast<int>(gid),
                     static_cast<unsigned long long>(chown_fail));
        }

        if (opt.do_selinux) {
            bool sel_ok = false;
            if (data_type_is_external(e.type)) {
                // 外部存储 label 由 file_contexts 决定，直接 restorecon
                sel_ok = selinux_restorecon(dest, true, true);
            } else {
                if (!e.context.empty()) {
                    sel_ok = selinux_set_context(dest, e.context, true);
                }
                if (!sel_ok) {
                    std::string derived = selinux_derive_app_context(dest);
                    if (!derived.empty()) sel_ok = selinux_set_context(dest, derived, true);
                }
                if (!sel_ok) sel_ok = selinux_restorecon(dest, true, true);
            }
            if (!sel_ok) {
                log_warn("[%s] selinux relabel failed on %s", e.type.c_str(), dest.c_str());
            }
        }

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

        log_info("[%s] restored %llu files (%s) -> %s", e.type.c_str(),
                 static_cast<unsigned long long>(st.files), human_size(st.bytes).c_str(), dest.c_str());
    }

    log_info("restore finished: %s", failed == 0 ? "ok" : "with errors");
    return failed == 0 ? 0 : 1;
}

int run_list(const std::string& in_dir) {
    BackupMeta meta;
    if (!read_meta_file(join_path(in_dir, "meta.json"), &meta)) return 1;
    printf("%-8s %-8s %-10s %-12s %-40s\n", "TYPE", "EXISTS", "FILES", "SIZE", "ARCHIVE");
    for (const auto& e : meta.entries) {
        printf("%-8s %-8s %-10llu %-12s %-40s\n", e.type.c_str(), e.exists ? "yes" : "no",
               static_cast<unsigned long long>(e.files), human_size(e.bytes).c_str(), e.archive.c_str());
    }
    printf("\npkg=%s user=%d uid=%d compression=%s created=%s\n", meta.pkg.c_str(), meta.user, meta.uid,
           meta.compression.c_str(), meta.created_at.c_str());
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
        printf("%-8s %-60s gid=%-6d ctx=%s\n", t.c_str(), path.c_str(), static_cast<int>(gid_of_path(path)),
               selinux_get_context(path).c_str());
    }
    return 0;
}

}  // namespace ozj
