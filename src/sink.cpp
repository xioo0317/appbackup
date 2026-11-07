#include "sink.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>

#include "common.h"

namespace ozj {

namespace {

class PlainSink : public Sink {
public:
    explicit PlainSink(const std::string& path) : path_(path) {
        fp_ = fopen(path.c_str(), "wb");
    }
    ~PlainSink() override {
        if (fp_) fclose(fp_);
    }
    bool write(const void* data, size_t len) override {
        if (!fp_) return false;
        if (len == 0) return true;
        if (fwrite(data, 1, len, fp_) != len) return false;
        written_ += len;
        return true;
    }
    bool close() override {
        if (!fp_) return false;
        int rc = fclose(fp_);
        fp_ = nullptr;
        return rc == 0;
    }
    uint64_t written() const override { return written_; }

private:
    std::string path_;
    FILE* fp_ = nullptr;
    uint64_t written_ = 0;
};

class GzipSink : public Sink {
public:
    GzipSink(const std::string& path, int level) : path_(path) {
        // 用 "wb" + gzsetparams 控制级别；不写 gzip 头里的原始文件名，避免泄露路径
        fp_ = gzopen(path.c_str(), "wb");
        if (fp_) gzsetparams(fp_, level, Z_DEFAULT_STRATEGY);
    }
    ~GzipSink() override {
        if (fp_) gzclose(fp_);
    }
    bool write(const void* data, size_t len) override {
        if (!fp_) return false;
        if (len == 0) return true;
        const unsigned char* p = static_cast<const unsigned char*>(data);
        size_t left = len;
        while (left > 0) {
            unsigned int chunk = left > (1u << 20) ? (1u << 20) : static_cast<unsigned int>(left);
            int rc = gzwrite(fp_, p, chunk);
            if (rc <= 0) return false;
            p += chunk;
            left -= chunk;
        }
        written_ += len;
        return true;
    }
    bool close() override {
        if (!fp_) return false;
        int rc = gzclose(fp_);
        fp_ = nullptr;
        return rc == Z_OK;
    }
    uint64_t written() const override { return written_; }

private:
    std::string path_;
    gzFile fp_ = nullptr;
    uint64_t written_ = 0;
};

class PlainSource : public Source {
public:
    explicit PlainSource(const std::string& path) : path_(path) { fp_ = fopen(path.c_str(), "rb"); }
    ~PlainSource() override {
        if (fp_) fclose(fp_);
    }
    long read(void* buf, size_t len) override {
        if (!fp_) return -1;
        size_t n = fread(buf, 1, len, fp_);
        if (n == 0) return ferror(fp_) ? -1 : 0;
        return static_cast<long>(n);
    }
    bool close() override {
        if (!fp_) return false;
        int rc = fclose(fp_);
        fp_ = nullptr;
        return rc == 0;
    }

private:
    std::string path_;
    FILE* fp_ = nullptr;
};

class GzipSource : public Source {
public:
    explicit GzipSource(const std::string& path) : path_(path) { fp_ = gzopen(path.c_str(), "rb"); }
    ~GzipSource() override {
        if (fp_) gzclose(fp_);
    }
    long read(void* buf, size_t len) override {
        if (!fp_) return -1;
        unsigned int chunk = len > (1u << 20) ? (1u << 20) : static_cast<unsigned int>(len);
        int n = gzread(fp_, buf, chunk);
        if (n < 0) return -1;
        return n;
    }
    bool close() override {
        if (!fp_) return false;
        int rc = gzclose(fp_);
        fp_ = nullptr;
        return rc == Z_OK;
    }

private:
    std::string path_;
    gzFile fp_ = nullptr;
};

}  // namespace

std::unique_ptr<Sink> open_plain_sink(const std::string& path) {
    return std::unique_ptr<Sink>(new PlainSink(path));
}

std::unique_ptr<Sink> open_gzip_sink(const std::string& path, int level) {
    return std::unique_ptr<Sink>(new GzipSink(path, level));
}

std::unique_ptr<Source> open_plain_source(const std::string& path) {
    return std::unique_ptr<Source>(new PlainSource(path));
}

std::unique_ptr<Source> open_gzip_source(const std::string& path) {
    return std::unique_ptr<Source>(new GzipSource(path));
}

bool file_is_gzip(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    unsigned char magic[2] = {0, 0};
    size_t n = fread(magic, 1, 2, fp);
    fclose(fp);
    return n == 2 && magic[0] == 0x1f && magic[1] == 0x8b;
}

}  // namespace ozj
