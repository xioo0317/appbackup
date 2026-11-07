#pragma once

#include <string>
#include <vector>

#include "meta.h"

namespace ozj {

enum class KillMode { None, Am, Kill };

struct BackupOptions {
    std::string pkg;
    int user = 0;
    std::vector<std::string> types;   // 空表示全部
    std::string out_dir = ".";        // 备份根目录，会在其下建 <pkg>_<时间戳>
    std::string compression = "gzip"; // none | gzip
    KillMode kill = KillMode::None;
    bool keep_source_owner = true;    // 归档内保留原 uid/gid（恢复时再重映射）
    bool verbose = false;
};

// 返回进程退出码：0 成功
int run_backup(const BackupOptions& opt);

}  // namespace ozj
