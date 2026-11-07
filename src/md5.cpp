#include "md5.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <vector>

namespace appbackup {

namespace {

inline uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

const uint32_t kK[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

const int kS[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                    14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                    4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

}  // namespace

Md5::Md5() : bit_count_(0), buffer_len_(0), finished_(false) {
    state_[0] = 0x67452301;
    state_[1] = 0xefcdab89;
    state_[2] = 0x98badcfe;
    state_[3] = 0x10325476;
    memset(buffer_, 0, sizeof(buffer_));
}

void Md5::transform(const uint8_t block[64]) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++) {
        m[i] = static_cast<uint32_t>(block[i * 4]) | (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 16) | (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl(a + f + kK[i] + m[g], kS[i]);
        a = tmp;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void Md5::update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bit_count_ += static_cast<uint64_t>(len) * 8;
    while (len > 0) {
        size_t take = 64 - buffer_len_;
        if (take > len) take = len;
        memcpy(buffer_ + buffer_len_, p, take);
        buffer_len_ += take;
        p += take;
        len -= take;
        if (buffer_len_ == 64) {
            transform(buffer_);
            buffer_len_ = 0;
        }
    }
}

std::string Md5::hex_digest() {
    if (!finished_) {
        uint64_t bits = bit_count_;
        uint8_t pad[72];
        size_t pad_len = (buffer_len_ < 56) ? (56 - buffer_len_) : (120 - buffer_len_);
        memset(pad, 0, sizeof(pad));
        pad[0] = 0x80;
        for (int i = 0; i < 8; i++) pad[pad_len + i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFF);
        update(pad, pad_len + 8);
        finished_ = true;
        char buf[33];
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                snprintf(buf + i * 8 + j * 2, 3, "%02x", (state_[i] >> (8 * j)) & 0xFF);
            }
        }
        buf[32] = '\0';
        digest_ = buf;
    }
    return digest_;
}

std::string md5_of_file(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::string();
    Md5 md5;
    std::vector<char> buf(1 << 20);
    ssize_t n;
    while ((n = read(fd, buf.data(), buf.size())) > 0) {
        md5.update(buf.data(), static_cast<size_t>(n));
    }
    close(fd);
    if (n < 0) return std::string();
    return md5.hex_digest();
}

}  // namespace appbackup
