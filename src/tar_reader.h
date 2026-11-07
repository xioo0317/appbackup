#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>

#include "sink.h"

namespace appbackup {

struct ExtractOptions {
    std::string dest_dir;                        // 解包根目录（归档内路径相对它）
    std::vector<std::string> exclude_prefixes;   // 命中即跳过（整棵子树）
    std::vector<std::string> exclude_globs;      // basename 通配
    bool restore_owner = true;                   // 解包时按归档里的 uid/gid 设置属主
    bool preserve_mtime = true;
};

struct ExtractStats {
    uint64_t files = 0;
    uint64_t dirs = 0;
    uint64_t links = 0;
    uint64_t skipped = 0;
    uint64_t bytes = 0;
};

bool tar_extract(Source& src, const ExtractOptions& opt, ExtractStats* stats);

}  // namespace appbackup
