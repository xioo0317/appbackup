#pragma once

#include <sys/types.h>

#include <string>

namespace appbackup {

// 查包在当前设备上的 uid（安装后由 PackageManager 分配）。
// retries * delay_ms 用于等待刚 pm install 完的包被系统刷新出来，拿不到返回 -1。
int uid_for_package(const std::string& pkg, int user, int retries = 20, int delay_ms = 500);

// 取目录自身的 gid（外部存储 /data/media/<user>/Android/* 是 media_rw，不能套用 uid 当 gid）
gid_t gid_of_path(const std::string& path);

// 取包 APK 所在目录（pm path 的父目录），失败返回空
std::string package_source_dir(const std::string& pkg, int user);

// 包是否已安装
bool package_installed(const std::string& pkg, int user);

}  // namespace appbackup
