#pragma once

#include <string>
#include <vector>

namespace appbackup {

// 源目录清单指纹，口径与 DataBackup 的 IncrementalBackupUtil.computeSourceHash 一致：
//   find [-L] <src> [-maxdepth N] [( -path <prune> ) -prune -o] -type f -printf '%P|%s|%T@\n'
//     | LC_ALL=C sort | sha256sum
// 实现上不走 shell（Android 的 toybox find 未必支持 -printf），直接遍历后按字节序排序再算 SHA-256。
struct HashOptions {
    bool follow_symlinks = false;
    int max_depth = 0;                     // 0 = 不限；1 = 只看 src 顶层（APK 场景）
    std::vector<std::string> prune_names;  // src 下要整棵剪掉的子目录名（如 "cache"）
};

// 失败（读不到目录）返回空串，调用方按「已变化」走全量
std::string compute_source_hash(const std::string& src, const HashOptions& opt);

}  // namespace appbackup
