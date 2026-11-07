#include "fingerprint.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fsops.h"
#include "sha256.h"

namespace appbackup {

namespace {

struct Entry {
    std::string rel;
    unsigned long long size = 0;
    long long sec = 0;
    long nsec = 0;
};

bool is_pruned(const std::string& name, const std::vector<std::string>& prune) {
    for (const auto& p : prune) {
        if (name == p) return true;
    }
    return false;
}

bool walk(const std::string& root, const std::string& rel, int depth, const HashOptions& opt,
          std::vector<Entry>* out, bool* failed) {
    std::string dir = rel.empty() ? root : (root + "/" + rel);
    DIR* d = opendir(dir.c_str());
    if (!d) {
        *failed = true;
        return false;
    }
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        std::string child_rel = rel.empty() ? name : (rel + "/" + name);
        std::string child_abs = root + "/" + child_rel;

        struct stat st;
        int rc = opt.follow_symlinks ? stat(child_abs.c_str(), &st) : lstat(child_abs.c_str(), &st);
        if (rc != 0) {
            // 与 find 一致：读不到的条目跳过而不是整体失败（权限问题在 /data 上很常见）
            continue;
        }
        if (S_ISLNK(st.st_mode) && !opt.follow_symlinks) continue;

        if (S_ISDIR(st.st_mode)) {
            // prune 只作用于 src 的直接子目录（上游 pruneDirs 就是 <src>/<name>）
            if (rel.empty() && is_pruned(name, opt.prune_names)) continue;
            if (opt.max_depth > 0 && depth + 1 >= opt.max_depth) continue;
            walk(root, child_rel, depth + 1, opt, out, failed);
        } else if (S_ISREG(st.st_mode)) {
            if (opt.max_depth > 0 && depth + 1 > opt.max_depth) continue;
            Entry e;
            e.rel = child_rel;
            e.size = static_cast<unsigned long long>(st.st_size);
            e.sec = static_cast<long long>(st.st_mtim.tv_sec);
            e.nsec = static_cast<long>(st.st_mtim.tv_nsec);
            out->push_back(e);
        }
    }
    closedir(d);
    return true;
}

}  // namespace

std::string compute_source_hash(const std::string& src, const HashOptions& opt) {
    std::string root = src;
    while (root.size() > 1 && root.back() == '/') root.pop_back();

    std::vector<Entry> entries;
    bool failed = false;
    walk(root, std::string(), 0, opt, &entries, &failed);
    if (failed) return std::string();

    std::vector<std::string> lines;
    lines.reserve(entries.size());
    char buf[128];
    for (const auto& e : entries) {
        // 与 GNU find %T@ 的输出对齐：<sec>.<9 位纳秒>0
        snprintf(buf, sizeof(buf), "%lld.%09ld0", e.sec, e.nsec);
        std::string line = e.rel;
        line += '|';
        line += std::to_string(e.size);
        line += '|';
        line += buf;
        lines.push_back(std::move(line));
    }
    // LC_ALL=C sort 等价于 std::string 的字节序比较
    std::sort(lines.begin(), lines.end());

    Sha256 sha;
    for (const auto& l : lines) {
        sha.update(l.data(), l.size());
        sha.update("\n", 1);
    }
    return sha.hex_digest();
}

}  // namespace appbackup
