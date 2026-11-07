#include "dbformat.h"

#include <sys/stat.h>
#include <unistd.h>

#include <ctime>
#include <fstream>
#include <sstream>

#include "common.h"
#include "fsops.h"
#include "md5.h"
#include "proc.h"

namespace appbackup {

namespace {

long long now_millis() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

std::string json_escape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
            out += buf;
        } else {
            out += c;
        }
    }
    return out;
}

// 目录名形如 user_0 或 user_0@1699999999999
bool parse_version_dir_name(const std::string& name, int* user, long long* preserve_id) {
    if (!starts_with(name, "user_")) return false;
    std::string rest = name.substr(5);
    size_t at = rest.find('@');
    std::string user_part = at == std::string::npos ? rest : rest.substr(0, at);
    if (user_part.empty()) return false;
    for (char c : user_part) {
        if (c < '0' || c > '9') return false;
    }
    *user = atoi(user_part.c_str());
    if (at == std::string::npos) {
        *preserve_id = 0;
        return true;
    }
    std::string id_part = rest.substr(at + 1);
    if (id_part.empty()) return false;
    for (char c : id_part) {
        if (c < '0' || c > '9') return false;
    }
    *preserve_id = atoll(id_part.c_str());
    return true;
}

}  // namespace

bool is_databackup_dir(const std::string& dir) {
    if (path_exists(join_path(dir, "package_restore_config.json"))) return true;
    int user = 0;
    long long pid = 0;
    return parse_version_dir_name(basename_of(dir), &user, &pid);
}

std::string databackup_version_dir(const std::string& save_dir, const std::string& pkg, int user,
                                   long long preserve_id, const std::string& label) {
    std::string name = "user_" + std::to_string(user);
    if (preserve_id != 0) name += "@" + std::to_string(preserve_id);
    // 应用目录：有 label 时与上游一致用 <label>_<pkg>，否则退回 <pkg>
    std::string app_dir = pkg;
    if (!label.empty()) {
        app_dir = label + "_" + pkg;
    }
    return join_path(join_path(join_path(save_dir, "apps"), app_dir), name);
}

bool read_databackup_info(const std::string& dir, DbFormatInfo* out) {
    out->valid = false;
    int user = 0;
    long long pid = 0;
    if (parse_version_dir_name(basename_of(dir), &user, &pid)) {
        out->user = user;
        out->preserve_id = pid;
        // 上一级目录名是包名（新格式为 label_pkg，这里取最后一段下划线之后不可靠，
        // 真实包名以 json 为准，缺失时用目录名兜底）
        out->pkg = basename_of(dirname_of(dir));
    }

    std::string cfg = join_path(dir, "package_restore_config.json");
    if (!path_exists(cfg)) {
        out->valid = !out->pkg.empty();
        return out->valid;
    }
    std::string text = read_file_head(cfg, 1 << 20);
    if (text.empty()) {
        out->valid = !out->pkg.empty();
        return out->valid;
    }

    std::string pkg = json_get_string(text, "packageName");
    if (!pkg.empty()) out->pkg = pkg;
    std::string label = json_get_string(text, "label");
    if (!label.empty()) out->label = label;
    std::string vn = json_get_string(text, "versionName");
    if (!vn.empty()) out->version_name = vn;

    long long v = json_get_number(text, "versionCode");
    if (v > 0) out->version_code = v;
    v = json_get_number(text, "userId");
    if (v >= 0) out->user = static_cast<int>(v);
    v = json_get_number(text, "uid");
    if (v > 0) out->uid = static_cast<int>(v);
    v = json_get_number(text, "preserveId");
    if (v > 0) out->preserve_id = v;
    std::string ct = json_get_string(text, "compressionType");
    if (!ct.empty()) out->compression_type = ct;

    out->valid = !out->pkg.empty();
    return out->valid;
}

std::string find_databackup_archive(const std::string& dir, const std::string& type) {
    for (const char* suffix : {".tar.zst", ".tar.gz", ".tar", ".tar.lz4"}) {
        std::string p = join_path(dir, type + suffix);
        if (path_exists(p)) return p;
    }
    return std::string();
}

void fill_package_display_info(const std::string& pkg, int user, DbFormatInfo* info) {
    (void)user;
    // dumpsys 里能直接拿到的数值/时间字段
    ExecResult r = run_shell("dumpsys package " + pkg + " 2>/dev/null");
    if (r.code != 0 || trim(r.out).empty()) {
        r = run_shell("cmd package dump " + pkg + " 2>/dev/null");
    }
    auto grab_digits = [&](const char* key) -> long long {
        size_t pos = r.out.find(key);
        if (pos == std::string::npos) return 0;
        pos += strlen(key);
        size_t end = pos;
        while (end < r.out.size() && isdigit(static_cast<unsigned char>(r.out[end]))) end++;
        if (end == pos) return 0;
        return atoll(r.out.substr(pos, end - pos).c_str());
    };
    auto grab_token = [&](const char* key) -> std::string {
        size_t pos = r.out.find(key);
        if (pos == std::string::npos) return std::string();
        pos += strlen(key);
        size_t end = pos;
        while (end < r.out.size() && r.out[end] != ' ' && r.out[end] != '\t' && r.out[end] != '\r' &&
               r.out[end] != '\n') {
            end++;
        }
        return r.out.substr(pos, end - pos);
    };
    if (info->version_code == 0) info->version_code = grab_digits("versionCode=");
    if (info->version_name.empty()) info->version_name = grab_token("versionName=");
    if (info->first_install_time == 0) info->first_install_time = grab_digits("firstInstallTime=");
    if (info->last_update_time == 0) info->last_update_time = grab_digits("lastUpdateTime=");

    // label：dumpsys 给不到，从 /data/system/packages.xml 的 label="..." 尽力读取
    if (info->label.empty()) {
        ExecResult p = run_shell("grep -m1 'name=\"" + pkg + "\"' /data/system/packages.xml 2>/dev/null");
        if (p.code == 0 && !trim(p.out).empty()) {
            std::string line = p.out;
            size_t lp = line.find("label=\"");
            if (lp != std::string::npos) {
                lp += strlen("label=\"");
                size_t le = line.find('"', lp);
                if (le != std::string::npos) info->label = line.substr(lp, le - lp);
            }
        }
    }
}

bool write_md5_sidecar(const std::string& archive_path) {
    std::string md5 = md5_of_file(archive_path);
    if (md5.empty()) {
        log_warn("md5 failed: %s", archive_path.c_str());
        return false;
    }
    std::ofstream f(archive_path + ".md5", std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << md5;
    f.close();
    return true;
}

bool write_databackup_config(const std::string& dir, const DbFormatInfo& info,
                             const std::vector<DataEntry>& entries, const std::string& backup_dir) {
    auto state_of = [&](const std::string& type) -> const char* {
        for (const auto& e : entries) {
            if (e.type == type) return e.exists ? "Selected" : "Disabled";
        }
        return "Disabled";
    };
    auto bytes_of = [&](const std::string& type) -> uint64_t {
        for (const auto& e : entries) {
            if (e.type == type) return e.bytes;
        }
        return 0;
    };

    std::ostringstream os;
    os << "{\n";
    os << "  \"id\": 0,\n";
    os << "  \"indexInfo\": {\n";
    os << "    \"opType\": \"RESTORE\",\n";
    os << "    \"packageName\": \"" << json_escape(info.pkg) << "\",\n";
    os << "    \"userId\": " << info.user << ",\n";
    os << "    \"compressionType\": \"" << json_escape(info.compression_type.empty() ? "ZSTD" : info.compression_type)
       << "\",\n";
    os << "    \"preserveId\": " << info.preserve_id << ",\n";
    os << "    \"cloud\": \"\",\n";
    os << "    \"backupDir\": \"" << json_escape(backup_dir) << "\"\n";
    os << "  },\n";
    os << "  \"packageInfo\": {\n";
    os << "    \"label\": \"" << json_escape(info.label) << "\",\n";
    os << "    \"versionName\": \"" << json_escape(info.version_name) << "\",\n";
    os << "    \"versionCode\": " << info.version_code << ",\n";
    os << "    \"flags\": 0,\n";
    os << "    \"firstInstallTime\": " << info.first_install_time << ",\n";
    os << "    \"lastUpdateTime\": " << info.last_update_time << "\n";
    os << "  },\n";
    os << "  \"extraInfo\": {\n";
    os << "    \"uid\": " << (info.uid > 0 ? info.uid : -1) << ",\n";
    os << "    \"hasKeystore\": false,\n";
    os << "    \"permissions\": [],\n";
    os << "    \"ssaid\": \"\",\n";
    os << "    \"lastBackupTime\": " << now_millis() << ",\n";
    os << "    \"blocked\": false,\n";
    os << "    \"activated\": false,\n";
    os << "    \"firstUpdated\": true,\n";
    os << "    \"enabled\": true\n";
    os << "  },\n";
    os << "  \"dataStates\": {\n";
    os << "    \"apkState\": \"" << state_of(kDataTypeApk) << "\",\n";
    os << "    \"userState\": \"" << state_of(kDataTypeUser) << "\",\n";
    os << "    \"userDeState\": \"" << state_of(kDataTypeUserDe) << "\",\n";
    os << "    \"dataState\": \"" << state_of(kDataTypeData) << "\",\n";
    os << "    \"obbState\": \"" << state_of(kDataTypeObb) << "\",\n";
    os << "    \"mediaState\": \"" << state_of(kDataTypeMedia) << "\",\n";
    os << "    \"permissionState\": \"Disabled\",\n";
    os << "    \"ssaidState\": \"Disabled\"\n";
    os << "  },\n";
    auto stats_block = [&](const char* indent) {
        std::ostringstream b;
        b << indent << "\"apkBytes\": " << bytes_of(kDataTypeApk) << ",\n";
        b << indent << "\"dataBytes\": " << bytes_of(kDataTypeData) << ",\n";
        b << indent << "\"mediaBytes\": " << bytes_of(kDataTypeMedia) << ",\n";
        b << indent << "\"obbBytes\": " << bytes_of(kDataTypeObb) << ",\n";
        b << indent << "\"userBytes\": " << bytes_of(kDataTypeUser) << ",\n";
        b << indent << "\"userDeBytes\": " << bytes_of(kDataTypeUserDe) << "\n";
        return b.str();
    };
    os << "  \"storageStats\": {\n";
    os << "    \"appBytes\": 0,\n    \"cacheBytes\": 0,\n    \"dataBytes\": 0,\n    \"externalCacheBytes\": 0\n";
    os << "  },\n";
    os << "  \"dataStats\": {\n" << stats_block("    ") << "  },\n";
    os << "  \"displayStats\": {\n" << stats_block("    ") << "  }\n";
    os << "}\n";

    std::ofstream f(join_path(dir, "package_restore_config.json"), std::ios::binary | std::ios::trunc);
    if (!f) {
        log_error("cannot write package_restore_config.json in %s", dir.c_str());
        return false;
    }
    f << os.str();
    f.close();
    chmod(join_path(dir, "package_restore_config.json").c_str(), 0644);
    return true;
}

}  // namespace appbackup
