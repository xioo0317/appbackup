#pragma once

#include <string>

namespace appbackup {

// 工具链：tar / zstd。
//
// 为什么要自带二进制：归档字节由「哪个 tar 实现」决定。Android 系统 tar 是 toybox
// 精简版（不认 --totals、头部字段与 GNU tar 不同），而 DataBackup 上游自带一份
// 静态链接的 GNU tar + zstd，解压到私有目录后 PATH 前置（见其 BaseUtil.EnvInitializer）。
// 我们复刻同样的策略：把同样的 GNU tar / zstd 嵌进可执行文件，首次运行释放到磁盘，
// 这样产出的归档与上游同源。释放失败时回退系统 tar，并自动去掉不被支持的选项。
namespace toolchain {

// tar / zstd 可执行文件路径。优先返回已释放的内嵌副本，其次系统 PATH 上的名字。
// 返回空串表示彻底找不到（调用方应报错）。
std::string tar();
std::string zstd();

// tar 能力探测（首次调用探测一次并缓存；基于 --version/语法错误关键词判定）。
// 用于把上游指令里 GNU 专有选项在精简 tar 上自动降级。
bool tar_has_totals();            // --totals
bool tar_has_exclude();           // --exclude=
bool tar_has_follow_symlinks();   // -h
bool tar_has_recursive_unlink();  // --recursive-unlink

// 一行诊断：tar=<路径> totals=1 exclude=1 -h=1 recursive-unlink=0 zstd=<路径>
std::string describe();

// 清空缓存（测试用；配合 APPBACKUP_BIN_DIR 切换工具链）
void reset_cache();

}  // namespace toolchain
}  // namespace appbackup
