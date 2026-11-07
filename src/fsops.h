#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace appbackup {

bool path_exists(const std::string& path);
bool is_directory(const std::string& path);
bool is_symlink(const std::string& path);
uint64_t file_size(const std::string& path);       // stat 普通文件大小，失败返回 0
uint64_t dir_size(const std::string& path);       // 递归求和（不跟随符号链接）
std::string read_symlink(const std::string& path);
bool make_dirs(const std::string& path, mode_t mode = 0755);
std::string dirname_of(const std::string& path);
std::string basename_of(const std::string& path);
std::string join_path(const std::string& a, const std::string& b);

// 递归 lchown（不跟随符号链接），返回失败项数量
uint64_t chown_recursive(const std::string& root, uid_t uid, gid_t gid);

// 统计属主不是 uid 的条目数（含自身）
uint64_t count_not_owned_by(const std::string& root, uid_t uid);

// 清空目录第一层内容，但保留 keep 名单里的名字（如 lib / cache / code_cache / no_backup / .ota）
uint64_t clean_dir_children(const std::string& root, const std::vector<std::string>& keep);

// 列出目录下一层名字（不含 . 与 ..）
std::vector<std::string> list_dir(const std::string& path);

// 解析 /proc/self/... 之类不存在时返回空
std::string read_file_head(const std::string& path, size_t max_len = 4096);

}  // namespace appbackup
