#pragma once

#include <string>
#include <vector>

#include "meta.h"
#include "sink.h"

namespace appbackup {

enum class KillMode { None, Am, Kill };

// 备份只产出 DataBackup（Android-DataBackup-custom）的目录结构：
//   <out>/apps/<pkg>/user_<uid>/{apk,user,user_de,data,obb,media}.tar.<suffix>
//   <out>/apps/<pkg>/user_<uid>/<type>.tar.<suffix>.md5
//   <out>/apps/<pkg>/user_<uid>/<type>.tar.<suffix>.manifest
//   <out>/apps/<pkg>/user_<uid>/package_restore_config.json
struct BackupOptions {
    std::string pkg;
    int user = 0;
    std::vector<std::string> types;   // 空表示全部
    std::string out_dir = ".";        // 备份根目录（其下建 apps/<pkg>/user_<uid>）
    Compression compression = Compression::Zstd;
    int level = 1;                    // zstd 压缩等级，与 DataBackup 默认一致
    bool preserve = false;            // 已有主备份时先归档为 user_<uid>@<ts>
    bool incremental = true;          // 命中 .manifest 指纹则跳过重打
    KillMode kill = KillMode::None;
    bool verbose = false;
};

// 返回进程退出码：0 成功
int run_backup(const BackupOptions& opt);

}  // namespace appbackup
