#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace appbackup {

// 归档字节流写入端（原始 / gzip 两种实现）
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool write(const void* data, size_t len) = 0;
    virtual bool close() = 0;
    virtual uint64_t written() const = 0;
};

// 归档字节流读取端
class Source {
public:
    virtual ~Source() = default;
    // 返回读到的字节数，0 表示 EOF，-1 表示错误
    virtual long read(void* buf, size_t len) = 0;
    virtual bool close() = 0;
};

enum class Compression { Plain, Gzip, Zstd };

const char* compression_name(Compression c);
const char* compression_suffix(Compression c);   // "" / ".gz" / ".zst"

std::unique_ptr<Sink> open_plain_sink(const std::string& path);
std::unique_ptr<Sink> open_gzip_sink(const std::string& path, int level);
// 未编译 zstd 时返回 nullptr（日志会说明）
std::unique_ptr<Sink> open_zstd_sink(const std::string& path, int level);
// 统一入口：按 Compression 选实现
std::unique_ptr<Sink> open_sink(const std::string& path, Compression c, int level = 6);

std::unique_ptr<Source> open_plain_source(const std::string& path);
std::unique_ptr<Source> open_gzip_source(const std::string& path);
std::unique_ptr<Source> open_zstd_source(const std::string& path);
// 统一入口：先嗅探魔数（gzip / zstd / 裸 tar），再选实现
std::unique_ptr<Source> open_source_auto(const std::string& path);

// 读文件头判断压缩类型
bool file_is_gzip(const std::string& path);
bool file_is_zstd(const std::string& path);
Compression detect_compression(const std::string& path);

// 解析 --compress 参数：none/tar/plain、gzip/gz、zstd/zst
bool parse_compression(const std::string& name, Compression* out);

}  // namespace appbackup
