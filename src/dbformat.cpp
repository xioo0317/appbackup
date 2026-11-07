#include "dbformat.h"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

#include "common.h"
#include "fsops.h"
#include "md5.h"
#include "proc.h"

namespace ozj {

namespace {

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
                                   long long preserve_id) {
    std::string name = "user_" + std::to_string(user);
    if (preserve_id != 0) name += "@" + std::to_string(preserve_id);
    return join_path(join_path(join_path(save_dir, "apps"), pkg), name);
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
    ExecResult r = run_shell("dumpsys package " + pkg + " 2>/dev/null | head -80");
    if (r.code != 0 || trim(r.out).empty()) {
        r = run_shell("cmd package dump " + pkg + " 2>/dev/null | head -80");
    }
    for (const auto& raw : split(r.out, '\n')) {
        std::string line = trim(raw);
        auto grab = [&](const char* key) -> std::string {
            size_t pos = line.find(key);
            if (pos == std::string::npos) return std::string();
            pos += strlen(key);
            size_t end = pos;
            while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '\r') end++;
            return line.substr(pos, end - pos);
        };
        if (info->version_name.empty()) {
            std::string v = grab("versionName=");
            if (!v.empty()) info->version_name = v;
        }
        if (info->version_code == 0) {
            std::string v = grab("versionCode=");
            if (!v.empty()) info->version_code = atoll(v.c_str());
        }
        if (info->label.empty()) {
            std::string v = grab("labelRes=");
            (void)v;  // label 取不到就留空，DataBackup 允许空 label
        }
    }
    (void)user;
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
    os << "    \"firstInstallTime\": 0,\n";
    os << "    \"lastUpdateTime\": 0\n";
    os << "  },\n";
    os << "  \"extraInfo\": {\n";
    os << "    \"uid\": " << (info.uid > 0 ? info.uid : -1) << ",\n";
    os << "    \"hasKeystore\": false,\n";
    os << "    \"permissions\": [],\n";
    os << "    \"ssaid\": \"\",\n";
    os << "    \"lastBackupTime\": 0,\n";
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
        b << indent << "\"userBytes\": " << bytes_of(kDataTypeUser) << ",\n";
        b << indent << "\"userDeBytes\": " << bytes_of(kDataTypeUserDe) << ",\n";
        b << indent << "\"dataBytes\": " << bytes_of(kDataTypeData) << ",\n";
        b << indent << "\"obbBytes\": " << bytes_of(kDataTypeObb) << ",\n";
        b << indent << "\"mediaBytes\": " << bytes_of(kDataTypeMedia) << "\n";
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

}  // namespace ozj
