#pragma once

#include <cstdint>
#include <string>

namespace appbackup {

// 流式 MD5（备份完整性校验用，与 DataBackup 的 <归档>.md5 对齐）
class Md5 {
public:
    Md5();
    void update(const void* data, size_t len);
    // 返回 32 位小写十六进制
    std::string hex_digest();

private:
    void transform(const uint8_t block[64]);
    uint32_t state_[4];
    uint64_t bit_count_;
    uint8_t buffer_[64];
    size_t buffer_len_;
    bool finished_;
    std::string digest_;
};

// 计算整个文件的 MD5，失败返回空串
std::string md5_of_file(const std::string& path);

}  // namespace appbackup
