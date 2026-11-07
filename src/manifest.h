#pragma once

#include <string>
#include <vector>

namespace appbackup {

// DataBackup 的归档旁挂清单（`<归档>.manifest`），用于「源目录未变化则跳过重打」。
//
// 上游 IncrementalBackupUtil.serialize 的字节级格式（末尾带换行）：
//   v=1
//   hash=<find -printf '%P|%s|%T@\n' | LC_ALL=C sort | sha256sum>
//   ct=ZSTD
//   level=1
//   threads=2
//   follow=false
//   excl=<排除项用 | 连接>
struct ManifestParams {
    std::string ct;   // TAR / ZSTD / LZ4
    int level = 1;
    int threads = 1;
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
