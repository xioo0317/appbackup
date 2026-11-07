#pragma once

#include <string>

namespace ozj {

// 读符号链接自身（不跟随）的 SELinux context，失败返回空串
std::string selinux_get_context(const std::string& path);

// 递归设置 context（等价 chcon -hR）；内部走 lsetxattr，失败回退 chcon
bool selinux_set_context(const std::string& path, const std::string& context, bool recursive);

// 递归 restorecon（等价 restorecon -RF）；优先用 libselinux 的
// selinux_android_restorecon，拿不到符号就回退命令行
bool selinux_restorecon(const std::string& path, bool force, bool recursive);

// 兜底：取父目录 context 并把 system_data_file 换成 app_data_file
std::string selinux_derive_app_context(const std::string& dir);

}  // namespace ozj
