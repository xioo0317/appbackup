#pragma once

#include <string>
#include <vector>

#include "meta.h"
#include "sink.h"

namespace appbackup {

enum class KillMode { None, Am, Kill };

// 备份只产出 DataBackup 的目录结构：
//   <out>/apps/<label>_<pkg>/user_<uid>/{apk,user,user_de,data,obb,media}.tar.<suffix>
//   <out>/apps/<label>_<pkg>/user_<uid>/<type>.tar.<suffix>.md5 / .manifest
//   <out>/apps/<label>_<pkg>/user_<uid>/package_restore_config.json
// 应用目录前缀来自应用名（label），与上游 App 产物一致；拿不到 label 时退回纯 <pkg>。
struct BackupOptions {
    std::string pkg;
    int user = 0;
    std::vector<std::string> types;   // 空表示全部
    std::string out_dir = ".";        // 备份根目录
    // 应用目录命名：
    //   "auto"（默认）→ 用设备上探测到的应用名做前缀 <label>_<pkg>
    //   "plain"       → 纯 <pkg>（兼容开源 main 分支的 archivesRelativeDir）
    //   其它文本      → 用该文本做前缀
    std::string label = "auto";
    Compression compression = Compression::Zstd;
    int level = 1;                    // zstd 压缩等级，与 DataBackup 默认一致
    int threads = 2;                  // .manifest 的 threads=，上游样本普遍为 2
    bool preserve = false;            // 已有主备份时先归档为 user_<uid>@<ts>
    bool incremental = true;          // 命中 .manifest 指纹则跳过重打
    bool backup_permissions = true;   // config.dataStates.permissionState
    bool backup_ssaid = true;         // config.dataStates.ssaidState
    KillMode kill = KillMode::None;
    bool verbose = false;
};

// 返回进程退出码：0 成功
int run_backup(const BackupOptions& opt);

}  // namespace appbackup
