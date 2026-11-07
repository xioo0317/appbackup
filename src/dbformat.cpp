#include "dbformat.h"

#include <sys/stat.h>
#include <unistd.h>

#include <ctime>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
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

// 解码一个 UTF-8 码点；失败返回 false（按单字节丢弃处理）。
bool utf8_next(const std::string& s, size_t i, uint32_t* cp, size_t* len) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    auto cont = [&](size_t k) -> bool {
        return k < s.size() && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80;
    };
    if (c < 0x80) { *cp = c; *len = 1; return true; }
    if ((c & 0xE0) == 0xC0 && cont(i + 1)) {
        *cp = (static_cast<uint32_t>(c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
        *len = 2;
        return true;
    }
    if ((c & 0xF0) == 0xE0 && cont(i + 1) && cont(i + 2)) {
        *cp = (static_cast<uint32_t>(c & 0x0F) << 12) |
              ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
              (static_cast<unsigned char>(s[i + 2]) & 0x3F);
        *len = 3;
        return true;
    }
    if ((c & 0xF8) == 0xF0 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
        *cp = (static_cast<uint32_t>(c & 0x07) << 18) |
              ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
              ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
              (static_cast<unsigned char>(s[i + 3]) & 0x3F);
        *len = 4;
        return true;
    }
    return false;
}

// 与上游 PackageEntity.sanitizedLabel 完全一致：
//   label.replace(Regex("[^\\u4e00-\\u9fa5A-Za-z0-9_]"), "").trim('_')
// 即只保留中文/英文/数字/下划线，其余字符（含空格、斜杠、emoji）一律删除，再去掉首尾下划线。
std::string sanitize_label(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = 0;
        size_t len = 1;
        bool ok = utf8_next(s, i, &cp, &len);
        bool keep = false;
        if (ok) {
            if (len == 1) {
                char c = static_cast<char>(cp);
                keep = isalnum(static_cast<unsigned char>(c)) || c == '_';
            } else {
                keep = (cp >= 0x4E00 && cp <= 0x9FA5);
            }
        }
        if (keep) out.append(s, i, len);
        i += ok ? len : 1;
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

// 应用目录名可能是 <pkg>，也可能是 <label>_<pkg>。config 里的 packageName 才是准绳，
// 这里只在 config 缺失/损坏时做兜底：取最后一个下划线后、含 '.' 的那一段。
std::string pkg_from_app_dir(const std::string& name) {
    size_t p = name.rfind('_');
    if (p != std::string::npos) {
        std::string tail = name.substr(p + 1);
        if (tail.find('.') != std::string::npos) return tail;
    }
    return name;
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
    // 应用目录：有 label 时与上游 App 一致用 <label>_<pkg>，否则退回 <pkg>
    std::string app_dir = pkg;
    std::string clean = sanitize_label(label);
    if (!clean.empty()) {
        app_dir = clean + "_" + pkg;
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
        out->pkg = pkg_from_app_dir(basename_of(dirname_of(dir)));
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

// dumpsys package 的 firstInstallTime/lastUpdateTime 在部分 ROM 上是可读日期
// ("2026-11-07 12:34:56")，直接按数字读会只拿到年份 "2026"。这里两种都处理。
long long parse_pkg_time(const std::string& text, const char* key) {
    size_t pos = text.find(key);
    if (pos == std::string::npos) return 0;
    pos += strlen(key);
    size_t end = pos;
    while (end < text.size() && isdigit(static_cast<unsigned char>(text[end]))) end++;
    std::string num = text.substr(pos, end - pos);
    if (num.size() >= 12) return atoll(num.c_str());  // 已是 epoch ms

    // 可读日期：先做字符白名单再交给 date，避免把 dumpsys 文本拼进 shell
    size_t eol = text.find('\n', pos);
    std::string s = trim(text.substr(pos, (eol == std::string::npos ? text.size() : eol) - pos));
    bool safe = !s.empty();
    for (char c : s) {
        if (!(isdigit(static_cast<unsigned char>(c)) || c == '-' || c == ':' || c == ' ' || c == '+' ||
              c == '.')) {
            safe = false;
            break;
        }
    }
    if (!safe) return 0;
    ExecResult d = run_shell("date -d '" + s + "' +%s 2>/dev/null");
    if (d.code == 0) {
        long long sec = atoll(trim(d.out).c_str());
        if (sec > 1000000000LL) return sec * 1000LL;
    }
    return 0;
}

// 应用名（label）：只通过 sh 指令 / 系统接口 / 可引用的二进制工具获取，不解析任何 XML。
//
// Android 的 dumpsys / pm 只给出 labelRes（资源 ID）或 nonLocalizedLabel=null，
// 没有任何 shell 命令能直接返回资源型应用名。与上游
// PackageManager.getApplicationLabel() / ApplicationInfo.loadLabel(pm) 真正同源的，
// 是 APK 资源表里的 application label，由 aapt 读出来：
//     application-label:'Naraka: Bladepoint'
//     application-label-zh-CN:'永劫无间'
// 这里按设备 locale 优先取本地化变体，与上游 loadLabel() 行为一致。
// aapt 是可引用的二进制工具：可用 APPBACKUP_AAPT 显式指定，否则在 PATH 与
// root 常见工具目录 / 模块目录里查找。
std::string probe_app_label(const std::string& pkg) {
    // 1) pm path：系统指令取 APK 路径，base.apk 优先（split APK 时 label 在 base 里）
    std::string apk;
    ExecResult pm = run_shell("pm path " + sh_quote(pkg) + " 2>/dev/null");
    for (const auto& line : split(pm.out, '\n')) {
        std::string s = trim(line);
        if (s.compare(0, 8, "package:") == 0) s = trim(s.substr(8));
        if (s.empty()) continue;
        bool is_base = s.size() >= 8 && s.compare(s.size() - 8, 8, "base.apk") == 0;
        if (apk.empty() || is_base) apk = s;
    }

    // 2) 定位可引用的 aapt 二进制工具
    std::string aapt;
    const char* env_aapt = getenv("APPBACKUP_AAPT");
    if (env_aapt && *env_aapt) {
        aapt = env_aapt;
    } else {
        ExecResult f = run_shell(
            "for p in aapt aapt2"
            " /data/adb/ksu/bin/aapt /data/adb/ksu/bin/aapt2"
            " /data/adb/magisk/aapt /data/adb/magisk/aapt2"
            " /data/adb/ap/bin/aapt /data/adb/ap/bin/aapt2"
            " /data/adb/modules/*/system/bin/aapt /data/adb/modules/*/system/bin/aapt2"
            " /system/bin/aapt /system/bin/aapt2 /system/xbin/aapt"
            " /vendor/bin/aapt /data/local/tmp/aapt; do"
            " command -v \"$p\" >/dev/null 2>&1 && { command -v \"$p\"; break; };"
            " [ -x \"$p\" ] && { echo \"$p\"; break; }; done");
        std::vector<std::string> fl = split(f.out, '\n');
        aapt = trim(fl.empty() ? f.out : fl[0]);
    }

    // 3) aapt dump badging：按设备 locale 优先取本地化 label
    if (!aapt.empty() && !apk.empty()) {
        ExecResult b = run_shell(sh_quote(aapt) + " dump badging " + sh_quote(apk) +
                                 " 2>/dev/null | grep '^application-label'");
        std::vector<std::string> lines = split(b.out, '\n');

        // 设备 locale（zh-CN 等）；aapt 的 key 形如 application-label-zh-CN
        ExecResult gp = run_shell(
            "getprop persist.sys.locale 2>/dev/null; getprop ro.product.locale 2>/dev/null");
        std::vector<std::string> keys;
        for (const auto& l : split(gp.out, '\n')) {
            std::string t = trim(l);
            if (t.empty() || t == "null") continue;
            keys.push_back("application-label-" + t + ":");
            size_t dash = t.find('-');
            if (dash != std::string::npos) keys.push_back("application-label-" + t.substr(0, dash) + ":");
        }
        keys.push_back("application-label:");

        for (const auto& key : keys) {
            for (const auto& line : lines) {
                std::string s = trim(line);
                if (s.compare(0, key.size(), key) != 0) continue;
                size_t q1 = s.find('\'');
                if (q1 == std::string::npos) continue;
                size_t q2 = s.rfind('\'');
                if (q2 == std::string::npos || q2 <= q1 + 1) continue;
                std::string v = s.substr(q1 + 1, q2 - q1 - 1);
                if (!v.empty()) return v;
            }
        }
    }

    // 4) 系统接口兜底：cmd package dump / dumpsys package 的 nonLocalizedLabel=
    //    仅当应用把 label 写成字面字符串（非资源引用）时才有值
    for (const char* cmd : {"cmd package dump ", "dumpsys package "}) {
        ExecResult d = run_shell(std::string(cmd) + sh_quote(pkg) + " 2>/dev/null");
        size_t p = d.out.find("nonLocalizedLabel=");
        if (p == std::string::npos) continue;
        p += strlen("nonLocalizedLabel=");
        size_t e = p;
        while (e < d.out.size() && d.out[e] != ' ' && d.out[e] != '\n' && d.out[e] != '\r' &&
               d.out[e] != '\t') {
            e++;
        }
        std::string v = trim(d.out.substr(p, e - p));
        if (!v.empty() && v != "null") return v;
    }
    return std::string();
}

}  // namespace

void fill_package_display_info(const std::string& pkg, int user, DbFormatInfo* info) {
    (void)user;
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
    if (info->first_install_time == 0) info->first_install_time = parse_pkg_time(r.out, "firstInstallTime=");
    if (info->last_update_time == 0) info->last_update_time = parse_pkg_time(r.out, "lastUpdateTime=");
    // ApplicationInfo.flags：部分 ROM 的 dump 带数字形式，拿不到就留 0（恢复端仅用于判断系统应用）
    if (info->app_flags == 0) {
        for (const char* k : {"pkgFlags=", "applicationFlags=", "flags=0x"}) {
            size_t pos = r.out.find(k);
            if (pos == std::string::npos) continue;
            pos += strlen(k);
            if (std::string(k) == "flags=0x") {
                info->app_flags = static_cast<long long>(strtoll(r.out.substr(pos).c_str(), nullptr, 16));
            } else {
                info->app_flags = grab_digits(k);
            }
            if (info->app_flags > 0) break;
        }
    }
    if (info->label.empty()) info->label = probe_app_label(pkg);
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
        // 上游 getPermissions：mode = ops?.get(op) ?: AppOpsManager.MODE_IGNORED(1)。
        // 没有 AppOps 记录时默认 1（ignore），不是 null；模板里 21 项 mode 全是 1 就是这个默认值。
        int mode = 1;
        if (m && op_code_verified(pkg, m->code, m->op_name, &verified)) {
            op = m->code;
            auto om = op_mode.find(m->op_name);
            if (om != op_mode.end()) {
                int v = appops_mode_value(om->second);
                if (v >= 0) mode = v;
            }
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
        log_warn("SSAID: cannot read %s, skip", path.c_str());
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
            log_warn("SSAID: unrecognized %s layout, skip", path.c_str());
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
            log_warn("SSAID: cannot write %s", tmp.c_str());
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
    // 上游 PackageDataStates 的 8 个状态默认全 Selected，含义是「用户勾选了这项」，
    // 与「该类型源目录是否存在」无关：obb/media 源不存在时依旧是 Selected。
    // 恢复端 reload 时会按实际存在的归档重新覆盖 dataStates，所以备份端写 Selected 是安全的。
    // 只有用户显式 --types 排除了某类型，才写 Disabled。
    auto state_of = [&](const std::string& type) -> const char* {
        for (const auto& e : entries) {
            if (e.type == type) return "Selected";
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
    // 上游语义（backupApk/backupData 里 setDataBytes / setDisplayBytes）：
    //   dataStats    = calculateSize(src)  打包前源目录原始字节数（含随后被 --exclude 排除的项）
    //   displayStats = calculateSize(dst)  压缩后归档 .tar.zst 的文件大小
    // 实测三个样本：displayStats.<type>Bytes 与归档文件字节数逐个精确相等。
    auto stats_block = [&](bool raw) {
        auto pick = [&](const std::string& type) -> uint64_t {
            for (const auto& e : entries) {
                if (e.type != type) continue;
                if (!raw) return e.bytes;                        // displayStats：归档大小
                // 上游对 USER 刻意跳过 calculateSize（海量小文件遍历要几十秒且发热），
                // 用归档大小回填 dataBytes → dataStats.userBytes == displayStats.userBytes。
                // 三个独立样本都验证了这一点。
                if (e.type == kDataTypeUser) return e.bytes;
                if (e.src_bytes > 0) return e.src_bytes;         // dataStats：源目录大小
                return e.raw_bytes > 0 ? e.raw_bytes : e.bytes;  // 兜底：tar --totals 原始大小
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
