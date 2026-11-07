#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sink.h"  // Compression

namespace appbackup {

// 系统 tar + zstd 封装：完全复刻 Android-DataBackup 的上游指令，不手写 tar/zstd。
//
// 上游备份（PackagesBackupUtil / TarUtil）：
//   数据： tar --totals [--exclude=...] [-h] -cpf - -C <srcDir> -- <src> | zstd -r -T0 --ultra -q --priority=rt -<level>
//   APK ： cd <srcDir> && tar --totals -cpf - ./*.apk | zstd ... && cd /
// 上游恢复（PackagesRestoreUtil / TarUtil）：
//   恢复： zstd -d -c <src> | tar --totals [--exclude=...] [--recursive-unlink] -xmpf - -C <dst>
//
// 为保证退出码干净、跨 shell 可移植，这里先落临时 tar 文件再交给 zstd（与管道输出字节完全一致）。

struct SysBackupOpt {
    std::string srcDir;                 // -C 目录（数据的父目录；APK 为该 apk 所在目录）
    std::string src;                    // 数据：包名；APK："./*.apk"
    std::string dst;                    // 最终归档路径（含 .tar / .tar.zst）
    std::vector<std::string> excludes;  // 已是 "pkg/sub" 或 "Backup_*" 形式，直接作 --exclude=
    bool follow_symlinks = false;       // -h（默认关，对齐上游 readFollowSymlinks 默认）
    bool apk_mode = false;              // cd srcDir 后用 glob ./*.apk
    Compression compression = Compression::Zstd;
    int level = 1;                      // 与上游默认压缩等级 1 一致
};

// 返回是否成功；uncompressed_bytes 带回临时 tar 字节数（用于日志/统计）
bool sys_backup(const SysBackupOpt& o, uint64_t* uncompressed_bytes = nullptr);

struct SysRestoreOpt {
    std::string src;                   // 归档（tar 或 tar.zst，按魔数自动识别）
    std::string dstDir;                // -C 目录（解到父目录）
    std::vector<std::string> excludes; // 已是 "pkg/sub" / "Backup_*" 形式
    bool recursive_unlink = false;     // --recursive-unlink（clean 恢复时开启）
};

bool sys_restore(const SysRestoreOpt& o);

}  // namespace appbackup
