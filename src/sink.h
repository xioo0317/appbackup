#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace ozj {

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

std::unique_ptr<Sink> open_plain_sink(const std::string& path);
std::unique_ptr<Sink> open_gzip_sink(const std::string& path, int level);

std::unique_ptr<Source> open_plain_source(const std::string& path);
std::unique_ptr<Source> open_gzip_source(const std::string& path);

// 读文件头两字节判断是否 gzip
bool file_is_gzip(const std::string& path);

}  // namespace ozj
