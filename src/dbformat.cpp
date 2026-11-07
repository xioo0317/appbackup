#include "dbformat.h"

#include <sys/stat.h>
#include <unistd.h>

#include <ctime>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
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
    v = json_get_number(text, "flags");
    if (v > 0) out->app_flags = v;

    // extraInfo.permissions / ssaid（上游备份里就有，恢复时要按 dataStates 决定是否应用）
    out->permission_selected = json_get_string(text, "permissionState") == "Selected";
    out->ssaid_selected = json_get_string(text, "ssaidState") == "Selected";
    std::string ss = json_get_string(text, "ssaid");
    if (!ss.empty()) out->ssaid = ss;

    size_t pk = text.find("\"permissions\"");
    if (pk != std::string::npos) {
        size_t lb = text.find('[', pk);
        size_t rb = text.find(']', lb == std::string::npos ? pk : lb);
        if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
            std::string arr = text.substr(lb + 1, rb - lb - 1);
            size_t pos = 0;
            while ((pos = arr.find('{', pos)) != std::string::npos) {
                size_t end = arr.find('}', pos);
                if (end == std::string::npos) break;
                std::string obj = arr.substr(pos, end - pos + 1);
                PermEntry pe;
                pe.name = json_get_string(obj, "name");
                if (!pe.name.empty()) {
                    // isGranted 是布尔
                    size_t gp = obj.find("\"isGranted\"");
                    if (gp != std::string::npos) {
                        size_t colon = obj.find(':', gp);
                        size_t q = colon == std::string::npos ? std::string::npos
                                                              : obj.find_first_not_of(" \t\r\n", colon + 1);
                        pe.granted = q != std::string::npos && obj.compare(q, 4, "true") == 0;
                    }
                    long long ov = json_get_number(obj, "op");
                    if (ov >= 0 && ov <= 1000) pe.op = static_cast<int>(ov);
                    long long mv = json_get_number(obj, "mode");
                    if (mv >= 0 && mv <= 4) pe.mode = static_cast<int>(mv);
                    out->permissions.push_back(pe);
                }
                pos = end;
            }
        }
    }

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

namespace {

// 取 dumpsys 里某个段落的子行：header 行之后、缩进更深的连续行（遇到下级标题/更外层缩进即结束）
std::vector<std::string> section_lines(const std::string& text, const std::string& header) {
    std::vector<std::string> out;
    size_t pos = text.find(header);
    if (pos == std::string::npos) return out;
    size_t line_start = text.rfind('\n', pos);
    line_start = line_start == std::string::npos ? 0 : line_start + 1;
    size_t indent = 0;
    while (line_start + indent < text.size() && text[line_start + indent] == ' ') indent++;
    pos = text.find('\n', pos);
    if (pos == std::string::npos) return out;
    pos++;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        std::string t = trim(line);
        if (!t.empty()) {
            size_t ind = 0;
            while (ind < line.size() && line[ind] == ' ') ind++;
            if (ind <= indent) break;      // 回到同级或更外层
            if (t.back() == ':') break;    // 下一个子标题
            out.push_back(t);
        }
        pos = eol + 1;
    }
    return out;
}

std::string attr_of(const std::string& tag, const std::string& key) {
    std::string needle = key + "=\"";
    size_t p = tag.find(needle);
    if (p == std::string::npos) return std::string();
    p += needle.size();
    size_t e = tag.find('"', p);
    if (e == std::string::npos) return std::string();
    return tag.substr(p, e - p);
}

// appops 模式字符串 → AppOpsManager MODE_*
int appops_mode_value(const std::string& s) {
    if (s == "allow") return 0;       // MODE_ALLOWED
    if (s == "ignore") return 1;      // MODE_IGNORED
    if (s == "deny") return 2;        // MODE_ERRORED
    if (s == "default") return 3;     // MODE_DEFAULT
    if (s == "foreground") return 4;  // MODE_FOREGROUND
    return -1;
}

// 权限 → AppOps（名称 + 代码）。上游用隐藏 API permissionToOpCode 拿映射，
// 我们无法调隐藏 API，这里用 AOSP 的 op 表，并且**逐条用设备校验**：
// appops get <pkg> <code> 打印的 op 名必须与表中一致，不一致就当作 OP_NONE（宁缺勿错）。
struct OpMapEntry {
    const char* perm;
    const char* op_name;
    int code;
};

const OpMapEntry kOpTable[] = {
    {"android.permission.ACCESS_COARSE_LOCATION", "COARSE_LOCATION", 0},
    {"android.permission.ACCESS_FINE_LOCATION", "FINE_LOCATION", 1},
    {"android.permission.VIBRATE", "VIBRATE", 3},
    {"android.permission.READ_CONTACTS", "READ_CONTACTS", 4},
    {"android.permission.WRITE_CONTACTS", "WRITE_CONTACTS", 5},
    {"android.permission.READ_CALENDAR", "READ_CALENDAR", 8},
    {"android.permission.WRITE_CALENDAR", "WRITE_CALENDAR", 9},
    {"android.permission.POST_NOTIFICATIONS", "POST_NOTIFICATION", 11},
    {"android.permission.CALL_PHONE", "CALL_PHONE", 13},
    {"android.permission.READ_SMS", "READ_SMS", 14},
    {"android.permission.RECEIVE_SMS", "RECEIVE_SMS", 16},
    {"android.permission.SEND_SMS", "SEND_SMS", 19},
    {"android.permission.SYSTEM_ALERT_WINDOW", "SYSTEM_ALERT_WINDOW", 24},
    {"android.permission.CAMERA", "CAMERA", 26},
    {"android.permission.RECORD_AUDIO", "RECORD_AUDIO", 27},
    {"android.permission.READ_EXTERNAL_STORAGE", "READ_EXTERNAL_STORAGE", 59},
    {"android.permission.WRITE_EXTERNAL_STORAGE", "WRITE_EXTERNAL_STORAGE", 60},
    {"android.permission.REQUEST_INSTALL_PACKAGES", "REQUEST_INSTALL_PACKAGES", 66},
    {"android.permission.BLUETOOTH_SCAN", "BLUETOOTH_SCAN", 77},
    {"android.permission.READ_MEDIA_IMAGES", "READ_MEDIA_IMAGES", 85},
    {"android.permission.READ_MEDIA_VIDEO", "READ_MEDIA_VIDEO", 86},
    {"android.permission.READ_MEDIA_AUDIO", "READ_MEDIA_AUDIO", 87},
    {"android.permission.BLUETOOTH_CONNECT", "BLUETOOTH_CONNECT", 111},
    {"android.permission.READ_MEDIA_VISUAL_USER_SELECTED", "READ_MEDIA_VISUAL_USER_SELECTED", 123},
    {"android.permission.ACCESS_LOCAL_NETWORK", "ACCESS_LOCAL_NETWORK", 169},
};

const OpMapEntry* lookup_op(const std::string& perm) {
    for (const auto& e : kOpTable) {
        if (perm == e.perm) return &e;
    }
    return nullptr;
}

// 设备侧校验：该 code 对应的 op 名是否与期望一致（缓存，避免重复 shell）
bool op_code_verified(const std::string& pkg, int code, const std::string& expect_name,
                      std::map<int, std::string>* cache) {
    auto it = cache->find(code);
    if (it == cache->end()) {
        ExecResult r = run_shell("appops get '" + pkg + "' " + std::to_string(code) + " 2>/dev/null");
        // 输出可能带 "Uid 10473:" / "Package com.x:" 之类表头，只能在行首精确匹配 op 名
        std::string hit;
        for (const auto& raw : split(r.out, '\n')) {
            std::string t = trim(raw);
            if (starts_with(t, expect_name + ":")) {
                hit = expect_name;
                break;
            }
        }
        it = cache->emplace(code, hit).first;
    }
    return it->second == expect_name;
}

std::string ssaid_file(int user) {
    // 主机自测可用 APPBACKUP_SYS_USERS_DIR 注入假目录；设备上就是 /data/system/users
    const char* override_dir = getenv("APPBACKUP_SYS_USERS_DIR");
    std::string base = override_dir ? std::string(override_dir) : std::string("/data/system/users");
    return base + "/" + std::to_string(user) + "/settings_ssaid.xml";
}

}  // namespace

void fill_package_permissions(const std::string& pkg, int user, int uid, DbFormatInfo* info) {
    (void)user;
    (void)uid;
    info->permissions.clear();

    ExecResult r = run_shell("dumpsys package '" + pkg + "' 2>/dev/null");
    if (r.code != 0 || trim(r.out).empty()) {
        r = run_shell("cmd package dump '" + pkg + "' 2>/dev/null");
    }
    const std::string& dump = r.out;

    std::vector<std::string> requested = section_lines(dump, "requested permissions:");
    // runtime permissions 段：<name>: granted=true/false, flags=[ ... ]
    std::map<std::string, bool> runtime;
    for (const auto& line : section_lines(dump, "runtime permissions:")) {
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string name = trim(line.substr(0, c));
        if (name.empty()) continue;
        runtime[name] = line.find("granted=true") != std::string::npos;
    }
    if (requested.empty() && runtime.empty()) return;

    // appops get <pkg>：OP_NAME: allow / ignore / deny / default / foreground
    std::map<std::string, std::string> op_mode;
    ExecResult a = run_shell("appops get '" + pkg + "' 2>/dev/null");
    for (const auto& raw : split(a.out, '\n')) {
        std::string line = trim(raw);
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string name = trim(line.substr(0, c));
        std::string rest = trim(line.substr(c + 1));
        if (starts_with(rest, "mode=")) rest = trim(rest.substr(5));
        size_t semi = rest.find(';');
        if (semi != std::string::npos) rest = trim(rest.substr(0, semi));
        if (!name.empty()) op_mode[name] = rest;
    }

    std::map<int, std::string> verified;
    std::vector<std::string> names = requested;
    for (const auto& kv : runtime) {
        if (std::find(names.begin(), names.end(), kv.first) == names.end()) names.push_back(kv.first);
    }

    for (const auto& name : names) {
        auto rt = runtime.find(name);
        bool granted = rt != runtime.end() && rt->second;

        const OpMapEntry* m = lookup_op(name);
        int op = -1;
        int mode = -1;
        if (m && op_code_verified(pkg, m->code, m->op_name, &verified)) {
            op = m->code;
            auto om = op_mode.find(m->op_name);
            if (om != op_mode.end()) mode = appops_mode_value(om->second);
        }
        // 与上游过滤口径一致：能映射到 op 的，或运行时权限（危险权限）才收录
        if (op < 0 && rt == runtime.end()) continue;

        PermEntry e;
        e.name = name;
        e.granted = granted;
        e.op = op;
        e.mode = mode;
        info->permissions.push_back(e);
    }
}

std::string read_ssaid_value(int user, int uid) {
    std::string text = read_file_head(ssaid_file(user), 4 << 20);
    if (text.empty()) return std::string();
    std::string key = std::to_string(uid);
    size_t pos = 0;
    while ((pos = text.find("<setting", pos)) != std::string::npos) {
        size_t end = text.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = text.substr(pos, end - pos);
        if (attr_of(tag, "name") == key) return attr_of(tag, "value");
        pos = end;
    }
    return std::string();
}

bool write_ssaid_value(int user, int uid, const std::string& pkg, const std::string& value) {
    std::string path = ssaid_file(user);
    std::string text = read_file_head(path, 4 << 20);
    if (text.empty()) {
        log_warn("SSAID: 读不到 %s，跳过", path.c_str());
        return false;
    }
    std::string key = std::to_string(uid);
    bool patched = false;

    size_t pos = 0;
    while ((pos = text.find("<setting", pos)) != std::string::npos) {
        size_t end = text.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = text.substr(pos, end - pos);
        if (attr_of(tag, "name") == key) {
            size_t vp = tag.find("value=\"");
            if (vp == std::string::npos) break;
            size_t vs = vp + 7;
            size_t ve = tag.find('"', vs);
            if (ve == std::string::npos) break;
            std::string new_tag = tag.substr(0, vs) + value + tag.substr(ve);
            text.replace(pos, tag.size(), new_tag);
            patched = true;
            break;
        }
        pos = end;
    }

    if (!patched) {
        // 没有该 uid 的条目：在 </settings> 前插入一条（id 取现有最大值 +1）
        size_t close = text.rfind("</settings>");
        if (close == std::string::npos) {
            log_warn("SSAID: %s 结构不认识，跳过", path.c_str());
            return false;
        }
        long next_id = 0;
        size_t p = 0;
        while ((p = text.find("id=\"", p)) != std::string::npos) {
            p += 4;
            size_t e = text.find('"', p);
            if (e == std::string::npos) break;
            long id = atol(text.substr(p, e - p).c_str());
            if (id >= next_id) next_id = id + 1;
        }
        std::string entry = "  <setting id=\"" + std::to_string(next_id) + "\" name=\"" + key + "\" value=\"" +
                            value + "\" package=\"" + pkg + "\" />\n";
        text.insert(close, entry);
    }

    std::string tmp = path + ".appbackup.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            log_warn("SSAID: 无法写 %s", tmp.c_str());
            return false;
        }
        f << text;
    }
    // 保持 system:system + 0600，并让 SELinux 标签随父目录（settings provider 才能读）
    run_shell("chown system:system '" + tmp + "' 2>/dev/null; chmod 0600 '" + tmp +
              "'; restorecon '" + tmp + "' 2>/dev/null; mv -f '" + tmp + "' '" + path + "'");
    return true;
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
    os << "    \"flags\": " << info.app_flags << ",\n";
    os << "    \"firstInstallTime\": " << info.first_install_time << ",\n";
    os << "    \"lastUpdateTime\": " << info.last_update_time << "\n";
    os << "  },\n";
    os << "  \"extraInfo\": {\n";
    os << "    \"uid\": " << (info.uid > 0 ? info.uid : -1) << ",\n";
    os << "    \"hasKeystore\": false,\n";
    os << "    \"permissions\": [";
    if (info.permissions.empty()) {
        os << "],\n";
    } else {
        os << "\n";
        for (size_t i = 0; i < info.permissions.size(); i++) {
            const PermEntry& pe = info.permissions[i];
            os << "      {\n";
            os << "        \"name\": \"" << json_escape(pe.name) << "\",\n";
            os << "        \"isGranted\": " << (pe.granted ? "true" : "false") << ",\n";
            os << "        \"op\": " << pe.op << ",\n";
            // 上游 PackagePermission.mode 可空：未知时写 null，恢复端就不会去 setMode
            if (pe.mode >= 0) {
                os << "        \"mode\": " << pe.mode << "\n";
            } else {
                os << "        \"mode\": null\n";
            }
            os << "      }" << (i + 1 == info.permissions.size() ? "\n" : ",\n");
        }
        os << "    ],\n";
    }
    os << "    \"ssaid\": \"" << json_escape(info.ssaid) << "\",\n";
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
    os << "    \"permissionState\": \"" << (info.permission_selected ? "Selected" : "Disabled") << "\",\n";
    os << "    \"ssaidState\": \"" << (info.ssaid_selected ? "Selected" : "Disabled") << "\"\n";
    os << "  },\n";
    // 上游语义：dataStats 是设备上的原始大小，displayStats 是归档（压缩后）大小
    auto stats_block = [&](bool raw) {
        auto pick = [&](const std::string& type) -> uint64_t {
            for (const auto& e : entries) {
                if (e.type != type) continue;
                return raw ? (e.raw_bytes > 0 ? e.raw_bytes : e.bytes) : e.bytes;
            }
            return 0;
        };
        std::ostringstream b;
        b << "    \"apkBytes\": " << pick(kDataTypeApk) << ",\n";
        b << "    \"dataBytes\": " << pick(kDataTypeData) << ",\n";
        b << "    \"mediaBytes\": " << pick(kDataTypeMedia) << ",\n";
        b << "    \"obbBytes\": " << pick(kDataTypeObb) << ",\n";
        b << "    \"userBytes\": " << pick(kDataTypeUser) << ",\n";
        b << "    \"userDeBytes\": " << pick(kDataTypeUserDe) << "\n";
        return b.str();
    };
    os << "  \"storageStats\": {\n";
    os << "    \"appBytes\": 0,\n    \"cacheBytes\": 0,\n    \"dataBytes\": 0,\n    \"externalCacheBytes\": 0\n";
    os << "  },\n";
    os << "  \"dataStats\": {\n" << stats_block(true) << "  },\n";
    os << "  \"displayStats\": {\n" << stats_block(false) << "  }\n";
    os << "}\n";

    std::ofstream f(join_path(dir, "package_restore_config.json"), std::ios::binary | std::ios::trunc);
    if (!f) {
        log_error("cannot write package_restore_config.json in %s", dir.c_str());
        return false;
    }
    f << os.str();
    f.close();
    chmod(join_path(dir, "package_restore_config.json").c_str(), 0666);  // 与上游 setAllPermissions 一致
    return true;
}

}  // namespace appbackup
