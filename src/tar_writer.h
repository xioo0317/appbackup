#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sink.h"

namespace ozj {

// 打包排除规则：
//  - exclude_prefixes：归档内路径前缀（目录整棵跳过），如 "com.foo/cache"
//  - exclude_globs：归档内 basename 通配（fnmatch），如 "Backup_*"、".trashed-*"
struct TarOptions {
    std::vector<std::string> exclude_prefixes;
    std::vector<std::string> exclude_globs;
    bool only_top_level_files = false;  // 只打包 root 下一层的普通文件（APK 场景）
    std::string file_suffix_filter;     // 仅 when only_top_level_files，如 ".apk"
    // 不写 root 目录本身，把 root 的子项直接放在归档顶层（DataBackup 的 apk.tar 就是 ./base.apk 这种）
    bool flat_root = false;
};

struct TarStats {
    uint64_t files = 0;
    uint64_t dirs = 0;
    uint64_t links = 0;
    uint64_t bytes = 0;
    uint64_t skipped = 0;
};

// 把 abs_root 这棵树写入 sink，归档内路径为 arc_prefix/...（arc_prefix 通常就是包名）
bool tar_add_tree(Sink& sink, const std::string& abs_root, const std::string& arc_prefix,
                  const TarOptions& opt, TarStats* stats);

// 写 tar 结束标记（两个全零块）
bool tar_write_end(Sink& sink);

// 判断某个归档内相对路径是否命中排除规则
bool tar_path_excluded(const std::string& arc_path, const TarOptions& opt);

}  // namespace ozj
