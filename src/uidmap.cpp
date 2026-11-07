#include "uidmap.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>

#include "common.h"
#include "fsops.h"
#include "proc.h"

namespace appbackup {

namespace {

// 从 "package:com.foo uid:10123" 里抠 uid
int parse_uid_line(const std::string& line, const std::string& pkg) {
    std::string needle = "package:" + pkg;
    size_t pos = line.find(needle);
    if (pos == std::string::npos) return -1;
    // 避免 com.foo 命中 com.foo.bar
    size_t after = pos + needle.size();
    if (after < line.size() && line[after] != ' ' && line[after] != '\t') return -1;
    size_t up = line.find("uid:", after);
    if (up == std::string::npos) return -1;
    up += 4;
    size_t end = up;
    while (end < line.size() && isdigit(static_cast<unsigned char>(line[end]))) end++;
    if (end == up) return -1;
    return atoi(line.substr(up, end - up).c_str());
}

}  // namespace

int uid_for_package(const std::string& pkg, int user, int retries, int delay_ms) {
    for (int i = 0; i < retries; i++) {
        std::vector<std::string> argv = {"pm", "list", "packages", "-U", "--user", std::to_string(user)};
        ExecResult r = run_exec(argv);
        if (r.code != 0) {
            // 部分 ROM 上 pm 需要走 cmd package
            r = run_exec({"cmd", "package", "list", "packages", "-U", "--user", std::to_string(user)});
        }
        for (const auto& line : split(r.out, '\n')) {
            int uid = parse_uid_line(trim(line), pkg);
            if (uid > 0) return uid;
        }
        if (i + 1 < retries) {
            usleep(static_cast<useconds_t>(delay_ms) * 1000);
        }
    }
    return -1;
}

gid_t gid_of_path(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return st.st_gid;
}

std::string package_source_dir(const std::string& pkg, int user) {
    std::vector<std::string> argv = {"pm", "path", "--user", std::to_string(user), pkg};
    ExecResult r = run_exec(argv);
    if (r.code != 0 || trim(r.out).empty()) {
        r = run_exec({"cmd", "package", "path", "--user", std::to_string(user), pkg});
    }
    for (const auto& line : split(r.out, '\n')) {
        std::string s = trim(line);
        if (starts_with(s, "package:")) {
            std::string apk = s.substr(strlen("package:"));
            return dirname_of(apk);
        }
    }
    return std::string();
}

bool package_installed(const std::string& pkg, int user) {
    return uid_for_package(pkg, user, 1, 0) > 0;
}

}  // namespace appbackup
