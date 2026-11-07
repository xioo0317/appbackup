#include "fsops.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "common.h"
#include "proc.h"

namespace appbackup {

bool path_exists(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

bool is_directory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

bool is_symlink(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return false;
    return S_ISLNK(st.st_mode);
}

std::string read_symlink(const std::string& path) {
    char buf[4096];
    ssize_t n = readlink(path.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    return std::string(buf);
}

std::string dirname_of(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return std::string(".");
    if (pos == 0) return std::string("/");
    return path.substr(0, pos);
}

std::string basename_of(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

bool make_dirs(const std::string& path, mode_t mode) {
    if (path.empty()) return false;
    if (is_directory(path)) return true;
    std::string parent = dirname_of(path);
    if (parent != path && parent != "." && !parent.empty()) {
        if (!make_dirs(parent, mode)) return false;
    }
    if (mkdir(path.c_str(), mode) == 0) return true;
    return errno == EEXIST;
}

uint64_t file_size(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_size);
}

uint64_t dir_size(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return 0;
    if (S_ISREG(st.st_mode)) return static_cast<uint64_t>(st.st_size);
    if (!S_ISDIR(st.st_mode)) return 0;
    uint64_t total = 0;
    DIR* d = opendir(path.c_str());
    if (!d) return 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        total += dir_size(join_path(path, e->d_name));
    }
    closedir(d);
    return total;
}

std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = opendir(path.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        out.push_back(e->d_name);
    }
    closedir(d);
    return out;
}

namespace {

uint64_t chown_walk(const std::string& path, uid_t uid, gid_t gid) {
    uint64_t failed = 0;
    if (lchown(path.c_str(), uid, gid) != 0) {
        log_debug("lchown failed: %s (%s)", path.c_str(), strerror(errno));
        failed++;
    }
    struct stat st;
    if (lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path.c_str());
        if (!d) return failed;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            failed += chown_walk(join_path(path, e->d_name), uid, gid);
        }
        closedir(d);
    }
    return failed;
}

uint64_t count_walk(const std::string& path, uid_t uid) {
    uint64_t n = 0;
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return 0;
    if (st.st_uid != uid) n++;
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path.c_str());
        if (!d) return n;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            n += count_walk(join_path(path, e->d_name), uid);
        }
        closedir(d);
    }
    return n;
}

}  // namespace

uint64_t chown_recursive(const std::string& root, uid_t uid, gid_t gid) {
    if (!path_exists(root)) return 0;
    return chown_walk(root, uid, gid);
}

uint64_t count_not_owned_by(const std::string& root, uid_t uid) {
    if (!path_exists(root)) return 0;
    return count_walk(root, uid);
}

uint64_t clean_dir_children(const std::string& root, const std::vector<std::string>& keep) {
    uint64_t removed = 0;
    for (const auto& name : list_dir(root)) {
        bool kept = false;
        for (const auto& k : keep) {
            if (name == k) {
                kept = true;
                break;
            }
        }
        if (kept) continue;
        std::string target = join_path(root, name);
        std::string cmd = "rm -rf '" + target + "'";
        if (run_shell(cmd).code == 0) removed++;
    }
    return removed;
}

std::string read_file_head(const std::string& path, size_t max_len) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return std::string();
    std::string out;
    out.resize(max_len);
    size_t n = fread(&out[0], 1, max_len, fp);
    fclose(fp);
    out.resize(n);
    return out;
}

}  // namespace appbackup
