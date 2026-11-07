#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace appbackup {

// 流式 SHA-256（DataBackup 的 .manifest 用 `find ... | sort | sha256sum` 的结果做增量指纹）
class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    // 返回 64 位小写十六进制
    std::string hex_digest();

private:
    void transform(const uint8_t block[64]);
    uint32_t state_[8];
    uint64_t bit_count_;
    uint8_t buffer_[64];
    size_t buffer_len_;
    bool finished_;
    std::string digest_;
};

}  // namespace appbackup
