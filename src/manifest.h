#pragma once

#include <string>
#include <vector>

namespace appbackup {

// DataBackup 的归档旁挂清单（`<归档>.manifest`），用于「源目录未变化则跳过重打」。
//
// 样本（上游与模板一致）的字节级格式（末尾带换行）：
//   v=1                 格式版本
//   hash=<源目录文件清单的 SHA-256>   注意：不是归档内容的哈希（那是 <归档>.md5）
//   ct=ZSTD             压缩类型，对应 indexInfo.compressionType
//   level=1             zstd 压缩等级（设置项 readCompressionLevel）
//   threads=2           压缩/任务线程参数（见下）
//   follow=false        是否跟随符号链接，对应 tar -h（设置项 readFollowSymlinks）
//   excl=<排除项用 | 连接>  对应 tar --exclude= 的同一份列表
//
// 这是一份「参数指纹」：源清单 hash + 打包参数全一致才跳过重打（manifest_params_equal）。
// 因此 threads/level/follow/excl 变化时也会触发全量重打，不是单纯的展示字段。
//
// threads 的来源：上游 zstd 命令是 `-T0`（0=自动用全部核心），所以这里的 2 并不是
// 传给 zstd 的 -T 值，而是该版本记入清单的一个压缩/任务参数（样本与用户模板普遍为 2）。
// 开源 main 分支没有生成 .manifest 的代码，故以样本为 ground truth；不同设备/设置/版本
// 可能取别的值，故做成可配置（--threads），默认 2 与样本对齐。
struct ManifestParams {
    std::string ct;   // TAR / ZSTD / LZ4
    int level = 1;
    int threads = 2;
    bool follow = false;
    std::vector<std::string> excl;
};

constexpr int kManifestVersion = 1;

std::string manifest_path(const std::string& archive);
std::string manifest_serialize(const std::string& hash, const ManifestParams& params);

// 解析失败（含版本不符）返回 false，调用方一律按「已变化」走全量
bool manifest_parse(const std::string& text, std::string* hash, ManifestParams* params);

bool manifest_write(const std::string& archive, const std::string& hash, const ManifestParams& params);
bool manifest_read(const std::string& archive, std::string* hash, ManifestParams* params);

bool manifest_params_equal(const ManifestParams& a, const ManifestParams& b);

}  // namespace appbackup
