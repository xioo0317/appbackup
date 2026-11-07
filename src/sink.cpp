#include "sink.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>

#if defined(APP_HAS_ZSTD)
#include <zstd.h>
#endif

#include "common.h"

namespace appbackup {

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

#if defined(APP_HAS_ZSTD)
class ZstdSink : public Sink {
public:
    ZstdSink(const std::string& path, int level) : path_(path) {
        fp_ = fopen(path.c_str(), "wb");
        if (!fp_) return;
        stream_ = ZSTD_createCStream();
        if (!stream_) {
            fclose(fp_);
            fp_ = nullptr;
            return;
        }
        size_t rc = ZSTD_initCStream(stream_, level);
        if (ZSTD_isError(rc)) {
            ZSTD_freeCStream(stream_);
            stream_ = nullptr;
            fclose(fp_);
            fp_ = nullptr;
            return;
        }
        out_.resize(ZSTD_CStreamOutSize());
    }
    ~ZstdSink() override {
        if (stream_) ZSTD_freeCStream(stream_);
        if (fp_) fclose(fp_);
    }
    bool write(const void* data, size_t len) override {
        if (!fp_ || !stream_) return false;
        if (len == 0) return true;
        ZSTD_inBuffer in{data, len, 0};
        while (in.pos < in.size) {
            ZSTD_outBuffer out{out_.data(), out_.size(), 0};
            size_t rc = ZSTD_compressStream(stream_, &out, &in);
            if (ZSTD_isError(rc)) return false;
            if (out.pos > 0) {
                if (fwrite(out_.data(), 1, out.pos, fp_) != out.pos) return false;
                written_ += out.pos;
            }
        }
        return true;
    }
    bool close() override {
        if (!fp_ || !stream_) return false;
        while (true) {
            ZSTD_outBuffer out{out_.data(), out_.size(), 0};
            size_t rc = ZSTD_endStream(stream_, &out);
            if (out.pos > 0) {
                if (fwrite(out_.data(), 1, out.pos, fp_) != out.pos) break;
                written_ += out.pos;
            }
            if (rc == 0) break;
            if (ZSTD_isError(rc)) break;
        }
        int rc = fclose(fp_);
        fp_ = nullptr;
        return rc == 0;
    }
    uint64_t written() const override { return written_; }

private:
    std::string path_;
    FILE* fp_ = nullptr;
    ZSTD_CStream* stream_ = nullptr;
    std::vector<char> out_;
    uint64_t written_ = 0;
};

class ZstdSource : public Source {
public:
    explicit ZstdSource(const std::string& path) : path_(path) {
        fp_ = fopen(path.c_str(), "rb");
        if (!fp_) return;
        stream_ = ZSTD_createDStream();
        if (!stream_) {
            fclose(fp_);
            fp_ = nullptr;
            return;
        }
        size_t rc = ZSTD_initDStream(stream_);
        if (ZSTD_isError(rc)) {
            ZSTD_freeDStream(stream_);
            stream_ = nullptr;
            fclose(fp_);
            fp_ = nullptr;
            return;
        }
        in_.resize(ZSTD_DStreamInSize());
        out_.resize(ZSTD_DStreamOutSize());
    }
    ~ZstdSource() override {
        if (stream_) ZSTD_freeDStream(stream_);
        if (fp_) fclose(fp_);
    }
    long read(void* buf, size_t len) override {
        if (!fp_ || !stream_) return -1;
        unsigned char* dst = static_cast<unsigned char*>(buf);
        size_t produced = 0;
        while (produced < len) {
            if (in_pos_ == in_len_) {
                in_len_ = fread(in_.data(), 1, in_.size(), fp_);
                in_pos_ = 0;
                if (in_len_ == 0) {
                    if (ferror(fp_)) return -1;
                    if (produced == 0) return eof_ ? 0 : (eof_ = true, 0);
                    return static_cast<long>(produced);
                }
            }
            ZSTD_inBuffer in{in_.data(), in_len_, in_pos_};
            size_t room = len - produced;
            size_t cap = room < out_.size() ? room : out_.size();
            ZSTD_outBuffer out{out_.data(), cap, 0};
            size_t rc = ZSTD_decompressStream(stream_, &out, &in);
            if (ZSTD_isError(rc)) return -1;
            in_pos_ = in.pos;
            if (out.pos > 0) {
                memcpy(dst + produced, out_.data(), out.pos);
                produced += out.pos;
            }
        }
        return static_cast<long>(produced);
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
    ZSTD_DStream* stream_ = nullptr;
    std::vector<char> in_;
    std::vector<char> out_;
    size_t in_len_ = 0;
    size_t in_pos_ = 0;
    bool eof_ = false;
};
#endif  // APP_HAS_ZSTD

}  // namespace

const char* compression_name(Compression c) {
    switch (c) {
        case Compression::Gzip: return "gzip";
        case Compression::Zstd: return "zstd";
        default: return "none";
    }
}

const char* compression_suffix(Compression c) {
    switch (c) {
        case Compression::Gzip: return ".gz";
        case Compression::Zstd: return ".zst";
        default: return "";
    }
}

bool parse_compression(const std::string& name, Compression* out) {
    if (name == "none" || name == "tar" || name == "plain" || name.empty()) {
        *out = Compression::Plain;
        return true;
    }
    if (name == "gzip" || name == "gz") {
        *out = Compression::Gzip;
        return true;
    }
    if (name == "zstd" || name == "zst") {
        *out = Compression::Zstd;
        return true;
    }
    return false;
}

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

std::unique_ptr<Sink> open_zstd_sink(const std::string& path, int level) {
#if defined(APP_HAS_ZSTD)
    return std::unique_ptr<Sink>(new ZstdSink(path, level));
#else
    (void)path;
    (void)level;
    log_error("此构建未启用 zstd（编译时缺 libzstd），请改用 --compress gzip");
    return nullptr;
#endif
}

std::unique_ptr<Sink> open_sink(const std::string& path, Compression c, int level) {
    switch (c) {
        case Compression::Gzip: return open_gzip_sink(path, level);
        case Compression::Zstd: return open_zstd_sink(path, level);
        default: return open_plain_sink(path);
    }
}

std::unique_ptr<Source> open_zstd_source(const std::string& path) {
#if defined(APP_HAS_ZSTD)
    return std::unique_ptr<Source>(new ZstdSource(path));
#else
    (void)path;
    log_error("此构建未启用 zstd，无法解压 .zst 归档");
    return nullptr;
#endif
}

std::unique_ptr<Source> open_source_auto(const std::string& path) {
    switch (detect_compression(path)) {
        case Compression::Gzip: return open_gzip_source(path);
        case Compression::Zstd: return open_zstd_source(path);
        default: return open_plain_source(path);
    }
}

bool file_is_gzip(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    unsigned char magic[2] = {0, 0};
    size_t n = fread(magic, 1, 2, fp);
    fclose(fp);
    return n == 2 && magic[0] == 0x1f && magic[1] == 0x8b;
}

bool file_is_zstd(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    unsigned char magic[4] = {0, 0, 0, 0};
    size_t n = fread(magic, 1, 4, fp);
    fclose(fp);
    // zstd 帧魔数 0xFD2FB528（小端存储为 28 B5 2F FD）
    return n == 4 && magic[0] == 0x28 && magic[1] == 0xB5 && magic[2] == 0x2F && magic[3] == 0xFD;
}

Compression detect_compression(const std::string& path) {
    if (file_is_gzip(path)) return Compression::Gzip;
    if (file_is_zstd(path)) return Compression::Zstd;
    return Compression::Plain;
}

}  // namespace appbackup
