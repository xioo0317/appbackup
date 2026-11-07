#include "tar_reader.h"

#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>

#include "common.h"
#include "fsops.h"
#include "tar_writer.h"  // 复用 tar_path_excluded

namespace appbackup {

namespace {

constexpr size_t kBlock = 512;

struct RawHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

static_assert(sizeof(RawHeader) == kBlock, "tar header must be 512 bytes");

bool read_exact(Source& src, void* buf, size_t len) {
    unsigned char* p = static_cast<unsigned char*>(buf);
    size_t left = len;
    while (left > 0) {
        long n = src.read(p, left);
        if (n <= 0) return false;
        p += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

bool skip_bytes(Source& src, uint64_t len) {
    unsigned char buf[64 * 1024];
    uint64_t left = len;
    while (left > 0) {
        size_t want = static_cast<size_t>(left > sizeof(buf) ? sizeof(buf) : left);
        if (!read_exact(src, buf, want)) return false;
        left -= want;
    }
    return true;
}

uint64_t parse_octal(const char* field, size_t len) {
    // GNU base-256 扩展：最高位为 1 表示二进制补码大端
    if (len > 0 && (field[0] & 0x80)) {
        uint64_t v = 0;
        for (size_t i = 1; i < len; i++) v = (v << 8) | static_cast<unsigned char>(field[i]);
        return v;
    }
    uint64_t v = 0;
    size_t i = 0;
    while (i < len && (field[i] == ' ' || field[i] == '\0')) i++;
    for (; i < len; i++) {
        char c = field[i];
        if (c < '0' || c > '7') break;
        v = v * 8 + static_cast<uint64_t>(c - '0');
    }
    return v;
}

std::string field_str(const char* field, size_t len) {
    size_t n = 0;
    while (n < len && field[n] != '\0') n++;
    return std::string(field, n);
}

// pax 记录解析： "<len> key=value\n"
void parse_pax(const std::string& data, std::map<std::string, std::string>* out) {
    size_t pos = 0;
    while (pos < data.size()) {
        size_t sp = data.find(' ', pos);
        if (sp == std::string::npos) break;
        size_t len = 0;
        try {
            len = static_cast<size_t>(std::stoul(data.substr(pos, sp - pos)));
        } catch (...) {
            break;
        }
        if (len == 0 || pos + len > data.size()) break;
        std::string rec = data.substr(sp + 1, pos + len - sp - 1);
        if (!rec.empty() && rec.back() == '\n') rec.pop_back();
        size_t eq = rec.find('=');
        if (eq != std::string::npos) {
            (*out)[rec.substr(0, eq)] = rec.substr(eq + 1);
        }
        pos += len;
    }
}

// 归一化归档内路径：去掉前导 "./" 与多余的 "/"，拒绝 .. 逃逸
bool normalize_arc_path(const std::string& in, std::string* out) {
    std::string s = in;
    while (starts_with(s, "./")) s = s.substr(2);
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s.empty()) return false;
    for (const auto& part : split(s, '/')) {
        if (part == "..") return false;
    }
    *out = s;
    return true;
}

bool apply_owner(const std::string& path, uid_t uid, gid_t gid) {
    return lchown(path.c_str(), uid, gid) == 0;
}

void apply_mtime(const std::string& path, int64_t mtime) {
    struct timespec times[2];
    times[0].tv_sec = static_cast<time_t>(mtime);
    times[0].tv_nsec = 0;
    times[1] = times[0];
    utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW);
}

bool ensure_parent(const std::string& path) {
    std::string parent = dirname_of(path);
    if (parent.empty() || parent == ".") return true;
    return make_dirs(parent, 0755);
}

}  // namespace

bool tar_extract(Source& src, const ExtractOptions& opt, ExtractStats* stats) {
    if (!make_dirs(opt.dest_dir, 0755)) {
        log_error("cannot create dest dir: %s", opt.dest_dir.c_str());
        return false;
    }

    std::map<std::string, std::string> pax_overrides;   // 作用于紧随其后的条目
    std::string long_name, long_link;

    while (true) {
        RawHeader h{};
        long n = src.read(&h, kBlock);
        if (n == 0) break;
        if (n != static_cast<long>(kBlock)) {
            log_error("truncated tar header");
            return false;
        }
        bool all_zero = true;
        const unsigned char* raw = reinterpret_cast<const unsigned char*>(&h);
        for (size_t i = 0; i < kBlock; i++) {
            if (raw[i] != 0) {
                all_zero = false;
                break;
            }
        }
        if (all_zero) break;  // 结束块

        char type = h.typeflag;
        uint64_t size = parse_octal(h.size, sizeof(h.size));

        if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
            std::string data;
            data.resize(static_cast<size_t>(size));
            if (size > 0 && !read_exact(src, &data[0], static_cast<size_t>(size))) return false;
            uint64_t pad = (kBlock - (size % kBlock)) % kBlock;
            if (pad > 0 && !skip_bytes(src, pad)) return false;
            if (type == 'x' || type == 'g') {
                parse_pax(data, &pax_overrides);
            } else if (type == 'L') {
                long_name = data;
                while (!long_name.empty() && long_name.back() == '\0') long_name.pop_back();
            } else {
                long_link = data;
                while (!long_link.empty() && long_link.back() == '\0') long_link.pop_back();
            }
            continue;
        }

        std::string name = field_str(h.name, sizeof(h.name));
        if (h.prefix[0] != '\0') name = field_str(h.prefix, sizeof(h.prefix)) + "/" + name;
        if (!long_name.empty()) name = long_name;
        if (!long_link.empty()) {
            // GNU 长链接名，用于 '2' / '1'
            std::string target = long_link;
            long_link.clear();
            long_name.clear();
            std::string norm;
            if (!normalize_arc_path(name, &norm)) {
                if (!skip_bytes(src, size)) return false;
                uint64_t pad = (kBlock - (size % kBlock)) % kBlock;
                if (pad > 0) skip_bytes(src, pad);
                continue;
            }
            std::string dst = join_path(opt.dest_dir, norm);
            ensure_parent(dst);
            unlink(dst.c_str());
            if (symlink(target.c_str(), dst.c_str()) != 0) {
                log_warn("symlink failed: %s (%s)", dst.c_str(), strerror(errno));
            } else {
                stats->links++;
            }
            if (!skip_bytes(src, size)) return false;
            uint64_t pad = (kBlock - (size % kBlock)) % kBlock;
            if (pad > 0) skip_bytes(src, pad);
            continue;
        }
        long_name.clear();

        auto it = pax_overrides.find("path");
        if (it != pax_overrides.end()) name = it->second;
        std::string link_name = field_str(h.linkname, sizeof(h.linkname));
        auto itl = pax_overrides.find("linkpath");
        if (itl != pax_overrides.end()) link_name = itl->second;
        pax_overrides.clear();

        std::string norm;
        if (!normalize_arc_path(name, &norm)) {
            log_warn("unsafe path in archive: %s", name.c_str());
            if (!skip_bytes(src, size)) return false;
            uint64_t pad = (kBlock - (size % kBlock)) % kBlock;
            if (pad > 0 && !skip_bytes(src, pad)) return false;
            continue;
        }

        TarOptions exclude_probe;
        exclude_probe.exclude_prefixes = opt.exclude_prefixes;
        exclude_probe.exclude_globs = opt.exclude_globs;
        if (tar_path_excluded(norm, exclude_probe)) {
            stats->skipped++;
            log_debug("skip %s", norm.c_str());
            if (!skip_bytes(src, size)) return false;
            uint64_t pad = (kBlock - (size % kBlock)) % kBlock;
            if (pad > 0 && !skip_bytes(src, pad)) return false;
            continue;
        }

        std::string dst = join_path(opt.dest_dir, norm);
        mode_t mode = static_cast<mode_t>(parse_octal(h.mode, sizeof(h.mode)) & 07777);
        uid_t uid = static_cast<uid_t>(parse_octal(h.uid, sizeof(h.uid)));
        gid_t gid = static_cast<gid_t>(parse_octal(h.gid, sizeof(h.gid)));
        int64_t mtime = static_cast<int64_t>(parse_octal(h.mtime, sizeof(h.mtime)));

        switch (type) {
            case '5': {  // 目录
                if (!ensure_parent(dst)) return false;
                if (!make_dirs(dst, mode ? mode : 0755)) {
                    log_warn("mkdir failed: %s (%s)", dst.c_str(), strerror(errno));
                }
                chmod(dst.c_str(), mode ? mode : 0755);
                if (opt.restore_owner) apply_owner(dst, uid, gid);
                if (opt.preserve_mtime) apply_mtime(dst, mtime);
                stats->dirs++;
                break;
            }
            case '2': {  // 符号链接
                if (!ensure_parent(dst)) return false;
                unlink(dst.c_str());
                if (symlink(link_name.c_str(), dst.c_str()) != 0) {
                    log_warn("symlink failed: %s (%s)", dst.c_str(), strerror(errno));
                } else {
                    if (opt.restore_owner) apply_owner(dst, uid, gid);
                    stats->links++;
                }
                break;
            }
            case '1': {  // 硬链接
                if (!ensure_parent(dst)) return false;
                std::string target_norm;
                std::string target = normalize_arc_path(link_name, &target_norm)
                                         ? join_path(opt.dest_dir, target_norm)
                                         : std::string();
                unlink(dst.c_str());
                if (!target.empty() && link(target.c_str(), dst.c_str()) == 0) {
                    stats->links++;
                } else {
                    log_warn("hardlink failed: %s -> %s (%s)", dst.c_str(), link_name.c_str(), strerror(errno));
                }
                break;
            }
            case '0':
            case '\0':
            case '7': {  // 普通文件
                if (!ensure_parent(dst)) return false;
                int fd = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode ? mode : 0644);
                if (fd < 0) {
                    log_warn("create failed: %s (%s)", dst.c_str(), strerror(errno));
                    if (!skip_bytes(src, size)) return false;
                    uint64_t pad = (kBlock - (size % kBlock)) % kBlock;
                    if (pad > 0 && !skip_bytes(src, pad)) return false;
                    break;
                }
                std::vector<char> buf(1 << 20);
                uint64_t left = size;
                bool ok = true;
                while (left > 0) {
                    size_t want = static_cast<size_t>(left > buf.size() ? buf.size() : left);
                    long got = src.read(buf.data(), want);
                    if (got <= 0) {
                        ok = false;
                        break;
                    }
                    size_t off = 0;
                    while (off < static_cast<size_t>(got)) {
                        ssize_t w = write(fd, buf.data() + off, static_cast<size_t>(got) - off);
                        if (w < 0) {
                            if (errno == EINTR) continue;
                            ok = false;
                            break;
                        }
                        off += static_cast<size_t>(w);
                    }
                    if (!ok) break;
                    left -= static_cast<uint64_t>(got);
                }
                close(fd);
                if (!ok) return false;
                uint64_t pad = (kBlock - (size % kBlock)) % kBlock;
                if (pad > 0 && !skip_bytes(src, pad)) return false;
                chmod(dst.c_str(), mode ? mode : 0644);
                if (opt.restore_owner) apply_owner(dst, uid, gid);
                if (opt.preserve_mtime) apply_mtime(dst, mtime);
                stats->files++;
                stats->bytes += size;
                break;
            }
            case '6': {  // FIFO
                if (!ensure_parent(dst)) return false;
                unlink(dst.c_str());
                mkfifo(dst.c_str(), mode ? mode : 0644);
                if (opt.restore_owner) apply_owner(dst, uid, gid);
                break;
            }
            default: {  // 其他类型：跳过数据
                if (!skip_bytes(src, size)) return false;
                uint64_t pad = (kBlock - (size % kBlock)) % kBlock;
                if (pad > 0 && !skip_bytes(src, pad)) return false;
                stats->skipped++;
                break;
            }
        }
    }
    return true;
}

}  // namespace appbackup
