#include "selinux.h"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common.h"
#include "fsops.h"
#include "proc.h"

namespace ozj {

namespace {

constexpr const char* kSelinuxXattr = "security.selinux";

// selinux/android.h 里的常量，不依赖 NDK 头文件
constexpr unsigned int kRestoreconForce = 8;
constexpr unsigned int kRestoreconRecurse = 4;

using RestoreconFn = int (*)(const char*, unsigned int);

RestoreconFn resolve_restorecon() {
    static RestoreconFn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        void* handle = dlopen("libselinux.so", RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            fn = reinterpret_cast<RestoreconFn>(dlsym(handle, "selinux_android_restorecon"));
            if (!fn) {
                log_debug("selinux_android_restorecon not found, will fallback to command");
            }
        } else {
            log_debug("dlopen libselinux.so failed: %s", dlerror());
        }
    }
    return fn;
}

bool set_one(const std::string& path, const std::string& context) {
    // lsetxattr 不跟随符号链接，等价 chcon -h
    if (lsetxattr(path.c_str(), kSelinuxXattr, context.c_str(), context.size(), 0) == 0) return true;
    return false;
}

}  // namespace

std::string selinux_get_context(const std::string& path) {
    char buf[512];
    ssize_t n = lgetxattr(path.c_str(), kSelinuxXattr, buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    return std::string(buf);
}

bool selinux_set_context(const std::string& path, const std::string& context, bool recursive) {
    if (context.empty()) return false;
    bool all_ok = true;
    if (!set_one(path, context)) {
        log_debug("lsetxattr failed on %s (%s)", path.c_str(), strerror(errno));
        all_ok = false;
    }
    struct stat st;
    if (recursive && lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path.c_str());
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                if (!selinux_set_context(join_path(path, e->d_name), context, true)) all_ok = false;
            }
            closedir(d);
        }
    }
    if (!all_ok) {
        // 回退到 chcon（部分 ROM 上 root 的 xattr 写入被拦，但 chcon 可用）
        std::string cmd = "chcon -hR '" + context + "' '" + path + "'";
        ExecResult r = run_shell(cmd);
        if (r.code == 0) {
            log_debug("chcon fallback ok: %s", path.c_str());
            return true;
        }
        log_warn("chcon failed: %s (%s)", path.c_str(), trim(r.out).c_str());
        return false;
    }
    return true;
}

bool selinux_restorecon(const std::string& path, bool force, bool recursive) {
    unsigned int flags = 0;
    if (recursive) flags |= kRestoreconRecurse;
    if (force) flags |= kRestoreconForce;

    RestoreconFn fn = resolve_restorecon();
    if (fn) {
        int rc = fn(path.c_str(), flags);
        if (rc == 0) return true;
        log_debug("selinux_android_restorecon rc=%d on %s", rc, path.c_str());
    }

    std::string cmd = std::string("restorecon ") + (recursive ? "-R " : "") + (force ? "-F " : "") + "'" + path + "'";
    ExecResult r = run_shell(cmd);
    if (r.code != 0) {
        log_warn("restorecon failed: %s (%s)", path.c_str(), trim(r.out).c_str());
        return false;
    }
    return true;
}

std::string selinux_derive_app_context(const std::string& dir) {
    std::string ctx = selinux_get_context(dir);
    if (ctx.empty()) return ctx;
    size_t pos = ctx.find("system_data_file");
    if (pos != std::string::npos) {
        ctx.replace(pos, strlen("system_data_file"), "app_data_file");
    }
    return ctx;
}

}  // namespace ozj
