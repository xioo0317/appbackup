#include "backup.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>

#include "common.h"
#include "fsops.h"
#include "proc.h"
#include "selinux.h"
#include "sink.h"
#include "tar_writer.h"
#include "uidmap.h"

namespace ozj {

namespace {

std::string timestamp_slug() {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

// DataBackup 的排除项唯一来源：
//   USER/USER_DE 排除 <pkg>/.ota|.cache|lib|code_cache|no_backup
//   DATA/OBB/MEDIA 排除 <pkg>/cache 及 Backup_*/.trashed-*/.pending-*
void fill_exclusions(const std::string& type, const std::string& pkg, TarOptions* opt) {
    if (type == kDataTypeUser || type == kDataTypeUserDe) {
        for (const char* sub : {".ota", "cache", "lib", "code_cache", "no_backup"}) {
            opt->exclude_prefixes.push_back(pkg + "/" + sub);
        }
    } else if (type == kDataTypeData || type == kDataTypeObb || type == kDataTypeMedia) {
        opt->exclude_prefixes.push_back(pkg + "/cache");
        opt->exclude_globs.push_back("Backup_*");
        opt->exclude_globs.push_back(".trashed-*");
        opt->exclude_globs.push_back(".pending-*");
    }
}

std::string archive_ext_for(const std::string& compression) {
    return compression == "gzip" ? ".tar.gz" : ".tar";
}

std::string read_prop(const std::string& key) {
    ExecResult r = run_shell("getprop " + key);
    return trim(r.out);
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
    if (opt.compression != "none" && opt.compression != "gzip") {
        log_error("unsupported compression: %s", opt.compression.c_str());
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

    std::string stamp = timestamp_slug();
    std::string dest = join_path(opt.out_dir, opt.pkg + "_" + stamp);
    if (!make_dirs(dest, 0755)) {
        log_error("cannot create %s", dest.c_str());
        return 1;
    }

    maybe_kill(opt.pkg, opt.user, opt.kill);

    BackupMeta meta;
    meta.tool = kToolName;
    meta.tool_version = kToolVersion;
    meta.pkg = opt.pkg;
    meta.user = opt.user;
    meta.uid = uid;
    meta.gid = uid;
    meta.created_at = now_iso8601();
    meta.device = read_prop("ro.product.model");
    meta.sdk = read_prop("ro.build.version.sdk");
    meta.compression = opt.compression;

    std::string ext = archive_ext_for(opt.compression);
    uint64_t total_bytes = 0;
    uint64_t total_files = 0;
    int failed = 0;

    for (const auto& type : types) {
        DataEntry entry;
        entry.type = type;
        entry.uid = uid;

        std::string src;
        if (type == kDataTypeApk) {
            std::string apk_dir = package_source_dir(opt.pkg, opt.user);
            if (apk_dir.empty()) {
                log_warn("apk source not found for %s", opt.pkg.c_str());
                entry.exists = false;
                meta.entries.push_back(entry);
                continue;
            }
            src = apk_dir;
            entry.arc_name = "apk";
        } else {
            std::string tmpl = data_type_dir_template(type);
            char buf[256];
            snprintf(buf, sizeof(buf), tmpl.c_str(), opt.user);
            src = join_path(buf, opt.pkg);
            entry.arc_name = opt.pkg;
        }
        entry.src_path = src;

        if (!path_exists(src)) {
            log_info("[%s] not present: %s", type.c_str(), src.c_str());
            entry.exists = false;
            meta.entries.push_back(entry);
            continue;
        }
        entry.exists = true;
        entry.context = selinux_get_context(src);
        entry.gid = static_cast<long>(gid_of_path(src));

        entry.archive = type + ext;
        std::string dst = join_path(dest, entry.archive);

        TarOptions topt;
        if (type == kDataTypeApk) {
            topt.only_top_level_files = true;
            topt.file_suffix_filter = ".apk";
        } else {
            fill_exclusions(type, opt.pkg, &topt);
        }

        std::unique_ptr<Sink> sink = opt.compression == "gzip" ? open_gzip_sink(dst, 6) : open_plain_sink(dst);
        if (!sink) {
            log_error("[%s] cannot open %s", type.c_str(), dst.c_str());
            failed++;
            meta.entries.push_back(entry);
            continue;
        }

        TarStats stats;
        bool ok = tar_add_tree(*sink, src, entry.arc_name, topt, &stats);
        ok = tar_write_end(*sink) && ok;
        ok = sink->close() && ok;

        entry.files = stats.files;
        entry.bytes = stats.bytes;
        total_files += stats.files;
        total_bytes += stats.bytes;
        if (!ok) {
            log_error("[%s] archive failed: %s", type.c_str(), dst.c_str());
            failed++;
        } else {
            log_info("[%s] %llu files, %s -> %s (%s)", type.c_str(), static_cast<unsigned long long>(stats.files),
                     human_size(stats.bytes).c_str(), entry.archive.c_str(),
                     human_size(static_cast<uint64_t>(file_size(dst))).c_str());
        }
        if (stats.skipped > 0) log_debug("[%s] skipped %llu entries", type.c_str(),
                                         static_cast<unsigned long long>(stats.skipped));
        meta.entries.push_back(entry);
    }

    if (!write_meta_file(join_path(dest, "meta.json"), meta)) {
        log_error("meta.json write failed");
        failed++;
    }

    log_info("backup %s -> %s : %llu files, %s, uid=%d", opt.pkg.c_str(), dest.c_str(),
             static_cast<unsigned long long>(total_files), human_size(total_bytes).c_str(), uid);
    return failed == 0 ? 0 : 1;
}

}  // namespace ozj
