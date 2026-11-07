// 主机侧自测：打包 / 解包往返 + 与系统 tar 互操作 + 路径穿越防护
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common.h"
#include "fsops.h"
#include "proc.h"
#include "sink.h"
#include "tar_reader.h"
#include "tar_writer.h"

using namespace ozj;

static int g_fail = 0;

static void check(bool cond, const char* what) {
    printf("%s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) g_fail++;
}

static void write_text(const std::string& path, const std::string& data) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    ssize_t n = write(fd, data.data(), data.size());
    (void)n;
    close(fd);
}

static std::string read_all(const std::string& path) {
    std::string out;
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    close(fd);
    return out;
}

static void rm_rf(const std::string& path) { run_shell("rm -rf '" + path + "'"); }

int main() {
    g_log_level = 1;
    std::string base = "/tmp/ozj_rt_" + std::to_string(getpid());
    std::string src = base + "/src";
    std::string out = base + "/out";
    rm_rf(base);

    make_dirs(src + "/com.demo.app/sub/deep", 0755);
    write_text(src + "/com.demo.app/a.txt", "hello ozj\n");
    write_text(src + "/com.demo.app/sub/b.bin", std::string(5000, 'x'));
    write_text(src + "/com.demo.app/empty.dat", "");
    write_text(src + "/com.demo.app/cache/should_skip.txt", "cache");
    write_text(src + "/com.demo.app/sub/deep/Backup_2024.db", "skip me");
    // 超长路径（>100 字节）触发 pax path 扩展
    std::string long_dir = src + "/com.demo.app/" + std::string(60, 'd') + "/" + std::string(60, 'e');
    make_dirs(long_dir, 0755);
    write_text(long_dir + "/" + std::string(90, 'f') + ".txt", "long path ok\n");
    // 符号链接 + 硬链接
    if (symlink("a.txt", (src + "/com.demo.app/link_to_a").c_str()) != 0) perror("symlink");
    if (link((src + "/com.demo.app/a.txt").c_str(), (src + "/com.demo.app/hard_to_a").c_str()) != 0) perror("link");

    TarOptions topt;
    topt.exclude_prefixes.push_back("com.demo.app/cache");
    topt.exclude_globs.push_back("Backup_*");

    std::string tar_path = base + "/t.tar";
    {
        std::unique_ptr<Sink> sink = open_plain_sink(tar_path);
        check(sink != nullptr, "open plain sink");
        TarStats st;
        bool ok = tar_add_tree(*sink, src + "/com.demo.app", "com.demo.app", topt, &st);
        ok = tar_write_end(*sink) && ok;
        ok = sink->close() && ok;
        check(ok, "tar_add_tree plain");
        check(st.files == 4, "file count (a.txt/b.bin/empty.dat/long.txt)");
        check(st.links == 2, "symlink + hardlink count");
        printf("       files=%llu dirs=%llu links=%llu bytes=%llu\n",
               static_cast<unsigned long long>(st.files), static_cast<unsigned long long>(st.dirs),
               static_cast<unsigned long long>(st.links), static_cast<unsigned long long>(st.bytes));
    }

    // 系统 tar 能读我们的产物
    {
        ExecResult r = run_shell("tar -tf '" + tar_path + "' >/dev/null 2>&1");
        check(r.code == 0, "system tar can list our archive");
        ExecResult r2 = run_shell("tar -tf '" + tar_path + "' | grep -c 'cache\\|Backup_'");
        check(trim(r2.out) == "0", "exclusions honoured (no cache/Backup_ entries)");
    }

    // 解包往返
    {
        ExtractOptions eopt;
        eopt.dest_dir = out;
        std::unique_ptr<Source> s = open_plain_source(tar_path);
        check(s != nullptr, "open plain source");
        ExtractStats est;
        bool ok = tar_extract(*s, eopt, &est);
        s->close();
        check(ok, "tar_extract plain");
        check(read_all(out + "/com.demo.app/a.txt") == "hello ozj\n", "content roundtrip");
        check(read_all(out + "/com.demo.app/sub/b.bin").size() == 5000, "binary size roundtrip");
        check(read_all(out + "/com.demo.app/empty.dat").empty(), "empty file");
        check(path_exists(out + "/com.demo.app/" + std::string(60, 'd') + "/" + std::string(60, 'e') + "/" +
                           std::string(90, 'f') + ".txt"),
              "long path (>255 bytes) roundtrip");
        struct stat st;
        check(lstat((out + "/com.demo.app/link_to_a").c_str(), &st) == 0 && S_ISLNK(st.st_mode), "symlink restored");
        check(lstat((out + "/com.demo.app/hard_to_a").c_str(), &st) == 0 && st.st_nlink == 2, "hardlink restored");
        check(!path_exists(out + "/com.demo.app/cache/should_skip.txt"), "excluded dir not extracted");
    }

    // gzip 往返
    {
        std::string gz = base + "/t.tar.gz";
        std::unique_ptr<Sink> sink = open_gzip_sink(gz, 6);
        TarStats st;
        bool ok = tar_add_tree(*sink, src + "/com.demo.app", "com.demo.app", topt, &st);
        ok = tar_write_end(*sink) && ok;
        ok = sink->close() && ok;
        check(ok, "gzip pack");
        check(file_is_gzip(gz), "gzip magic detected");

        std::string out2 = base + "/out2";
        ExtractOptions eopt;
        eopt.dest_dir = out2;
        std::unique_ptr<Source> s = open_gzip_source(gz);
        ExtractStats est;
        bool ok2 = s && tar_extract(*s, eopt, &est);
        if (s) s->close();
        check(ok2, "gzip unpack");
        check(read_all(out2 + "/com.demo.app/a.txt") == "hello ozj\n", "gzip content roundtrip");

        ExecResult r = run_shell("tar -tzf '" + gz + "' >/dev/null 2>&1");
        check(r.code == 0, "system tar can read our .tar.gz");
    }

    // 路径穿越防护
    {
        std::string evil = base + "/evil.tar";
        // 手工构造一个 ../escape.txt 的头
        std::string payload = "pwned\n";
        char h[512];
        memset(h, 0, sizeof(h));
        memcpy(h, "../escape.txt", 13);
        snprintf(h + 100, 8, "%07o", 0644);
        snprintf(h + 108, 8, "%07o", 0);
        snprintf(h + 116, 8, "%07o", 0);
        snprintf(h + 124, 12, "%011o", static_cast<unsigned>(payload.size()));
        snprintf(h + 136, 12, "%011o", 0);
        memset(h + 148, ' ', 8);
        h[156] = '0';
        memcpy(h + 257, "ustar", 5);
        memcpy(h + 263, "00", 2);
        unsigned sum = 0;
        for (size_t i = 0; i < 512; i++) sum += static_cast<unsigned char>(h[i]);
        snprintf(h + 148, 8, "%06o", sum);
        std::string blob(h, 512);
        blob.append(payload);
        blob.append(512 - payload.size(), '\0');
        blob.append(1024, '\0');
        write_text(evil, blob);

        std::string out3 = base + "/out3";
        ExtractOptions eopt;
        eopt.dest_dir = out3;
        std::unique_ptr<Source> s = open_plain_source(evil);
        ExtractStats est;
        bool ok = s && tar_extract(*s, eopt, &est);
        if (s) s->close();
        check(ok, "traversal archive handled without error");
        check(!path_exists(base + "/escape.txt"), "path traversal blocked (no ../escape.txt)");
    }

    rm_rf(base);
    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
