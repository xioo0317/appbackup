#include "tar_writer.h"

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>

#include "common.h"
#include "fsops.h"

namespace ozj {

namespace {

constexpr size_t kBlock = 512;

struct TarHeader {
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

static_assert(sizeof(TarHeader) == kBlock, "tar header must be 512 bytes");

void write_octal(char* field, size_t len, uint64_t value) {
    // len 含末尾 NUL；按 tar 规范写入零填充的八进制，超出字段宽度就饱和到最大值
    size_t width = len - 1;
    uint64_t max_value = 0;
    for (size_t i = 0; i < width; i++) max_value = (max_value << 3) | 7ULL;
    if (value > max_value) value = max_value;
    for (size_t i = width; i > 0; i--) {
        field[i - 1] = static_cast<char>('0' + (value & 7ULL));
        value >>= 3;
    }
    field[width] = '\0';
}

bool write_all(Sink& sink, const void* data, size_t len) {
    return sink.write(data, len);
}

bool write_padding(Sink& sink, uint64_t size) {
    uint64_t rem = size % kBlock;
    if (rem == 0) return true;
    char zeros[kBlock] = {0};
    return sink.write(zeros, static_cast<size_t>(kBlock - rem));
}

// pax 扩展头：写 path / linkpath 这类超长字段
bool write_pax(Sink& sink, const std::string& arc_name, const std::map<std::string, std::string>& kv) {
    std::string payload;
    for (const auto& item : kv) {
        std::string record = " " + item.first + "=" + item.second + "\n";
        // 长度字段本身也计入长度，需要迭代到稳定
        size_t len = record.size();
        while (true) {
            std::string prefix = std::to_string(len) + " ";
            if (prefix.size() + record.size() - 1 == len) break;
            len = prefix.size() + record.size() - 1;
        }
        payload += std::to_string(len) + " " + item.first + "=" + item.second + "\n";
    }

    TarHeader h{};
    std::string pax_name = arc_name;
    // pax 头自身的名字取不超过 100 字节的截断版本，真实名字在 payload 里
    std::string header_name = "./PaxHeaders/" + basename_of(arc_name);
    if (header_name.size() >= sizeof(h.name)) header_name = header_name.substr(0, sizeof(h.name) - 1);
    memcpy(h.name, header_name.c_str(), header_name.size());
    write_octal(h.mode, sizeof(h.mode), 0644);
    write_octal(h.uid, sizeof(h.uid), 0);
    write_octal(h.gid, sizeof(h.gid), 0);
    write_octal(h.size, sizeof(h.size), payload.size());
    write_octal(h.mtime, sizeof(h.mtime), 0);
    h.typeflag = 'x';
    memcpy(h.magic, "ustar", 5);
    memcpy(h.version, "00", 2);
    memset(h.chksum, ' ', sizeof(h.chksum));
    unsigned int sum = 0;
    const unsigned char* raw = reinterpret_cast<const unsigned char*>(&h);
    for (size_t i = 0; i < kBlock; i++) sum += raw[i];
    snprintf(h.chksum, sizeof(h.chksum), "%06o", sum);
    h.chksum[6] = '\0';
    h.chksum[7] = ' ';

    if (!write_all(sink, &h, kBlock)) return false;
    if (!payload.empty() && !write_all(sink, payload.data(), payload.size())) return false;
    return write_padding(sink, payload.size());
}

bool write_header(Sink& sink, const std::string& arc_name, const std::string& link_name, char type,
                  mode_t mode, uid_t uid, gid_t gid, uint64_t size, int64_t mtime) {
    bool need_pax = arc_name.size() >= sizeof(TarHeader::name) || link_name.size() >= sizeof(TarHeader::linkname);
    if (need_pax) {
        std::map<std::string, std::string> kv;
        if (arc_name.size() >= sizeof(TarHeader::name)) kv["path"] = arc_name;
        if (link_name.size() >= sizeof(TarHeader::linkname)) kv["linkpath"] = link_name;
        if (!write_pax(sink, arc_name, kv)) return false;
    }

    TarHeader h{};
    std::string short_name = arc_name;
    if (short_name.size() >= sizeof(h.name)) short_name = short_name.substr(0, sizeof(h.name) - 1);
    memcpy(h.name, short_name.c_str(), short_name.size());
    write_octal(h.mode, sizeof(h.mode), mode & 07777);
    write_octal(h.uid, sizeof(h.uid), uid);
    write_octal(h.gid, sizeof(h.gid), gid);
    write_octal(h.size, sizeof(h.size), size);
    write_octal(h.mtime, sizeof(h.mtime), mtime < 0 ? 0 : static_cast<uint64_t>(mtime));
    h.typeflag = type;
    if (!link_name.empty()) {
        std::string short_link = link_name;
        if (short_link.size() >= sizeof(h.linkname)) short_link = short_link.substr(0, sizeof(h.linkname) - 1);
        memcpy(h.linkname, short_link.c_str(), short_link.size());
    }
    memcpy(h.magic, "ustar", 5);
    memcpy(h.version, "00", 2);
    memset(h.chksum, ' ', sizeof(h.chksum));
    unsigned int sum = 0;
    const unsigned char* raw = reinterpret_cast<const unsigned char*>(&h);
    for (size_t i = 0; i < kBlock; i++) sum += raw[i];
    snprintf(h.chksum, sizeof(h.chksum), "%06o", sum);
    h.chksum[6] = '\0';
    h.chksum[7] = ' ';
    return write_all(sink, &h, kBlock);
}

bool copy_file_data(Sink& sink, const std::string& abs_path, uint64_t size, uint64_t* written) {
    int fd = open(abs_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        log_warn("open failed: %s (%s)", abs_path.c_str(), strerror(errno));
        return false;
    }
    std::vector<char> buf(1 << 20);
    uint64_t left = size;
    bool ok = true;
    while (left > 0) {
        size_t want = static_cast<size_t>(left > buf.size() ? buf.size() : left);
        ssize_t n = read(fd, buf.data(), want);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_warn("read failed: %s (%s)", abs_path.c_str(), strerror(errno));
            ok = false;
            break;
        }
        if (n == 0) break;  // 文件在打包过程中被截断
        if (!sink.write(buf.data(), static_cast<size_t>(n))) {
            ok = false;
            break;
        }
        left -= static_cast<uint64_t>(n);
        *written += static_cast<uint64_t>(n);
    }
    // 文件实际比头里写的小：用零补齐，保证 tar 结构正确
    if (left > 0) {
        std::vector<char> zeros(static_cast<size_t>(left > (1 << 20) ? (1 << 20) : left), 0);
        while (left > 0) {
            size_t n = static_cast<size_t>(left > zeros.size() ? zeros.size() : left);
            if (!sink.write(zeros.data(), n)) {
                ok = false;
                break;
            }
            left -= n;
        }
    }
    close(fd);
    return ok;
}

struct WalkCtx {
    Sink* sink = nullptr;
    const TarOptions* opt = nullptr;
    TarStats* stats = nullptr;
    std::map<std::pair<dev_t, ino_t>, std::string> hardlinks;
    bool ok = true;
};

void walk(WalkCtx& ctx, const std::string& abs_path, const std::string& arc_path, bool top_level) {
    if (!ctx.ok) return;
    struct stat st;
    if (lstat(abs_path.c_str(), &st) != 0) {
        log_warn("lstat failed: %s (%s)", abs_path.c_str(), strerror(errno));
        return;
    }

    if (tar_path_excluded(arc_path, *ctx.opt)) {
        ctx.stats->skipped++;
        log_debug("skip %s", arc_path.c_str());
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        std::string dir_name = arc_path + "/";
        if (!write_header(*ctx.sink, dir_name, "", '5', st.st_mode, st.st_uid, st.st_gid, 0, st.st_mtime)) {
            ctx.ok = false;
            return;
        }
        ctx.stats->dirs++;
        DIR* d = opendir(abs_path.c_str());
        if (!d) {
            log_warn("opendir failed: %s (%s)", abs_path.c_str(), strerror(errno));
            return;
        }
        std::vector<std::string> names;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            names.emplace_back(e->d_name);
        }
        closedir(d);
        for (const auto& n : names) {
            if (ctx.opt->only_top_level_files && !top_level) continue;
            walk(ctx, join_path(abs_path, n), arc_path + "/" + n, false);
            if (!ctx.ok) return;
        }
        return;
    }

    if (S_ISLNK(st.st_mode)) {
        std::string target = read_symlink(abs_path);
        if (!write_header(*ctx.sink, arc_path, target, '2', st.st_mode, st.st_uid, st.st_gid, 0, st.st_mtime)) {
            ctx.ok = false;
            return;
        }
        ctx.stats->links++;
        return;
    }

    if (S_ISREG(st.st_mode)) {
        if (ctx.opt->only_top_level_files && !top_level) return;
        if (ctx.opt->only_top_level_files && !ctx.opt->file_suffix_filter.empty() &&
            !ends_with(arc_path, ctx.opt->file_suffix_filter)) {
            ctx.stats->skipped++;
            return;
        }
        // 硬链接去重：同一 inode 只存一份数据
        if (st.st_nlink > 1) {
            auto key = std::make_pair(st.st_dev, st.st_ino);
            auto it = ctx.hardlinks.find(key);
            if (it != ctx.hardlinks.end()) {
                if (!write_header(*ctx.sink, arc_path, it->second, '1', st.st_mode, st.st_uid, st.st_gid, 0,
                                  st.st_mtime)) {
                    ctx.ok = false;
                    return;
                }
                ctx.stats->links++;
                return;
            }
            ctx.hardlinks[key] = arc_path;
        }
        uint64_t size = static_cast<uint64_t>(st.st_size);
        if (!write_header(*ctx.sink, arc_path, "", '0', st.st_mode, st.st_uid, st.st_gid, size, st.st_mtime)) {
            ctx.ok = false;
            return;
        }
        uint64_t written = 0;
        if (!copy_file_data(*ctx.sink, abs_path, size, &written)) {
            ctx.ok = false;
            return;
        }
        if (!write_padding(*ctx.sink, size)) {
            ctx.ok = false;
            return;
        }
        ctx.stats->files++;
        ctx.stats->bytes += size;
        return;
    }

    if (S_ISFIFO(st.st_mode)) {
        if (!write_header(*ctx.sink, arc_path, "", '6', st.st_mode, st.st_uid, st.st_gid, 0, st.st_mtime)) {
            ctx.ok = false;
        }
        return;
    }
    // 套接字/设备节点跳过
    ctx.stats->skipped++;
}

}  // namespace

bool tar_path_excluded(const std::string& arc_path, const TarOptions& opt) {
    for (const auto& p : opt.exclude_prefixes) {
        if (arc_path == p) return true;
        if (starts_with(arc_path, p + "/")) return true;
    }
    if (!opt.exclude_globs.empty()) {
        std::string base = basename_of(arc_path);
        for (const auto& g : opt.exclude_globs) {
            if (fnmatch(g.c_str(), base.c_str(), 0) == 0) return true;
        }
    }
    return false;
}

bool tar_add_tree(Sink& sink, const std::string& abs_root, const std::string& arc_prefix,
                  const TarOptions& opt, TarStats* stats) {
    WalkCtx ctx;
    ctx.sink = &sink;
    ctx.opt = &opt;
    ctx.stats = stats;
    if (opt.flat_root) {
        // 扁平模式：root 自身不入归档，子项直接以 ./name 形式出现在顶层
        std::vector<std::string> names = list_dir(abs_root);
        for (const auto& n : names) {
            walk(ctx, join_path(abs_root, n), "./" + n, true);
            if (!ctx.ok) break;
        }
    } else {
        walk(ctx, abs_root, arc_prefix, true);
    }
    return ctx.ok;
}

bool tar_write_end(Sink& sink) {
    char zeros[kBlock * 2] = {0};
    return sink.write(zeros, sizeof(zeros));
}

}  // namespace ozj
