#pragma once

#include <string>
#include <vector>

#include "meta.h"
#include "sink.h"

namespace appbackup {

enum class KillMode { None, Am, Kill };

struct BackupOptions {
    std::string pkg;
    int user = 0;
    std::vector<std::string> types;   // 空表示全部
    std::string out_dir = ".";        // 备份根目录
    Compression compression = Compression::Zstd;
    KillMode kill = KillMode::None;
    // true：写成 DataBackup 兼容结构 <out>/apps/<pkg>/user_<uid>@<ts>/<type>.tar.zst + package_restore_config.json
    bool databackup_format = false;
    bool verbose = false;
};

// 返回进程退出码：0 成功
int run_backup(const BackupOptions& opt);

}  // namespace appbackup
