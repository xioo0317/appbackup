#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sink.h"  // Compression

namespace appbackup {

// tar + zstd 封装：复刻 Android-DataBackup 的上游指令，不手写 tar/zstd。
//
// 上游备份（TarUtil / PackagesBackupUtil）：
//   数据： tar --totals [--exclude=...] [-h] -cpf - -C <srcDir> -- <src> | zstd ... > <dst>
//   APK ： cd <srcDir> && tar --totals -cpf - ./*.apk | zstd ... > <dst>
// 上游恢复（TarUtil / PackagesRestoreUtil）：
//         zstd -d -c <src> | tar --totals [--exclude=...] [--recursive-unlink] -xmpf - -C <dst>
//
// 必须用「管道」而不是「先落 tar 文件再压缩」：
//   1. zstd 从 stdin 读时不知道输入长度，帧头不写 contentSize；从普通文件读会写，
//      两者字节不同（这正是之前 data.tar.zst 相差若干字节的原因之一）。
//   2. 上游 --totals 的统计走 stderr，我们据此得到未压缩字节数（dataStats）。
//
// 工具链来自 toolchain::（内嵌 GNU tar / zstd，运行期释放）；若退化到系统 toybox tar，
// 不支持的选项会被自动去掉，不会像以前那样整条命令被拒。

struct SysBackupOpt {
    std::string srcDir;                 // -C 目录（数据的父目录；APK 为该 apk 所在目录）
    std::string src;                    // 数据：包名；APK 模式忽略（自动列 srcDir 下 *.apk）
    std::string dst;                    // 最终归档路径（.tar / .tar.zst）
    std::vector<std::string> excludes;  // 已是 "pkg/sub" 或 "Backup_*" 形式，直接作 --exclude=
    bool follow_symlinks = false;       // -h（默认关，对齐上游 readFollowSymlinks 默认）
    bool apk_mode = false;              // 只打 srcDir 顶层的 *.apk
    Compression compression = Compression::Zstd;
    int level = 1;                      // 与上游默认压缩等级 1 一致
    bool* skipped = nullptr;            // APK 模式下目录里没有 .apk 时置 true（不算失败）
};

// 返回是否成功；uncompressed_bytes 带回 tar --totals 报告的字节数（拿不到则为 0）
bool sys_backup(const SysBackupOpt& o, uint64_t* uncompressed_bytes = nullptr);

struct SysRestoreOpt {
    std::string src;                   // 归档（tar 或 tar.zst，按魔数自动识别）
    std::string dstDir;                // -C 目录（解到父目录）
    std::vector<std::string> excludes; // 已是 "pkg/sub" / "Backup_*" 形式
    bool recursive_unlink = false;     // --recursive-unlink（clean 恢复时开启）
};

bool sys_restore(const SysRestoreOpt& o);

}  // namespace appbackup
