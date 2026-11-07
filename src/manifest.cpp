#include "manifest.h"

#include <fstream>
#include <sstream>

#include "common.h"
#include "fsops.h"

namespace appbackup {

namespace {

std::string join_excl(const std::vector<std::string>& list) {
    std::string out;
    for (size_t i = 0; i < list.size(); ++i) {
        if (i) out += '|';
        out += list[i];
    }
    return out;
}

std::vector<std::string> split_excl(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '|') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace

std::string manifest_path(const std::string& archive) { return archive + ".manifest"; }

// 与 IncrementalBackupUtil.serialize 逐字节对齐：字段顺序固定，每行以 \n 结尾（含最后一行）。
std::string manifest_serialize(const std::string& hash, const ManifestParams& params) {
    std::ostringstream os;
    os << "v=" << kManifestVersion << "\n";
    os << "hash=" << hash << "\n";
    os << "ct=" << params.ct << "\n";
    os << "level=" << params.level << "\n";
    os << "threads=" << params.threads << "\n";
    os << "follow=" << (params.follow ? "true" : "false") << "\n";
    os << "excl=" << join_excl(params.excl) << "\n";
    return os.str();
}

bool manifest_parse(const std::string& text, std::string* hash, ManifestParams* params) {
    std::vector<std::pair<std::string, std::string>> kv;
    for (const auto& raw : split(text, '\n')) {
        std::string line = trim(raw);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    auto get = [&](const std::string& key, std::string* out) -> bool {
        for (const auto& p : kv) {
            if (p.first == key) {
                *out = p.second;
                return true;
            }
        }
        return false;
    };
    std::string v;
    if (!get("v", &v) || atoi(v.c_str()) != kManifestVersion) return false;
    std::string h;
    if (!get("hash", &h) || h.empty()) return false;
    std::string ct;
    if (!get("ct", &ct)) return false;  // ct 缺失拒收，宁可全量重打
    ManifestParams p;
    p.ct = ct;
    std::string level;
    if (!get("level", &level)) return false;
    p.level = atoi(level.c_str());
    std::string threads;
    if (!get("threads", &threads)) return false;
    p.threads = atoi(threads.c_str());
    std::string follow;
    get("follow", &follow);
    p.follow = follow == "true";
    std::string excl;
    get("excl", &excl);
    p.excl = split_excl(excl);

    *hash = h;
    *params = p;
    return true;
}

bool manifest_write(const std::string& archive, const std::string& hash, const ManifestParams& params) {
    std::ofstream f(manifest_path(archive), std::ios::binary | std::ios::trunc);
    if (!f) {
        log_warn("cannot write manifest: %s", manifest_path(archive).c_str());
        return false;
    }
    f << manifest_serialize(hash, params);
    f.close();
    return true;
}

bool manifest_read(const std::string& archive, std::string* hash, ManifestParams* params) {
    std::string p = manifest_path(archive);
    if (!path_exists(p)) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream os;
    os << f.rdbuf();
    return manifest_parse(os.str(), hash, params);
}

bool manifest_params_equal(const ManifestParams& a, const ManifestParams& b) {
    return a.ct == b.ct && a.level == b.level && a.threads == b.threads && a.follow == b.follow &&
           a.excl == b.excl;
}

}  // namespace appbackup
