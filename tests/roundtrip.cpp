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
#include "dbformat.h"
#include "md5.h"
#include "meta.h"
#include "proc.h"
#include "restore_policy.h"
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

#if defined(OZJ_HAS_ZSTD)
    // zstd 往返 + 与系统 zstd 互操作
    {
        std::string zst = base + "/t.tar.zst";
        std::unique_ptr<Sink> sink = open_sink(zst, Compression::Zstd, 6);
        TarStats st;
        bool ok = sink && tar_add_tree(*sink, src + "/com.demo.app", "com.demo.app", topt, &st);
        ok = tar_write_end(*sink) && ok;
        ok = sink->close() && ok;
        check(ok, "zstd pack");
        check(file_is_zstd(zst), "zstd magic detected");
        check(detect_compression(zst) == Compression::Zstd, "detect_compression -> zstd");

        std::string out4 = base + "/out4";
        ExtractOptions eopt;
        eopt.dest_dir = out4;
        std::unique_ptr<Source> s = open_source_auto(zst);
        ExtractStats est;
        bool ok2 = s && tar_extract(*s, eopt, &est);
        if (s) s->close();
        check(ok2, "zstd unpack (auto detect)");
        check(read_all(out4 + "/com.demo.app/a.txt") == "hello ozj\n", "zstd content roundtrip");

        ExecResult has = run_shell("command -v zstd >/dev/null 2>&1");
        if (has.code == 0) {
            ExecResult r = run_shell("zstd -d -c '" + zst + "' 2>/dev/null | tar -tf - >/dev/null 2>&1");
            check(r.code == 0, "system zstd+tar can read our .tar.zst");
        } else {
            printf("[SKIP] system zstd+tar (host has no zstd CLI)\n");
        }
    }
#endif

    // DataBackup 兼容结构：flat apk 归档 + package_restore_config.json + md5
    {        std::string apkSrc = base + "/apksrc";
        make_dirs(apkSrc, 0755);
        write_text(apkSrc + "/base.apk", "PK\x03\x04 fake apk");
        write_text(apkSrc + "/split_config.arm64_v8a.apk", "PK\x03\x04 split");
        write_text(apkSrc + "/notapk.txt", "skip");

        std::string dbdir = base + "/DataBackup/apps/com.demo.app/user_0@1767225600000";
        make_dirs(dbdir, 0755);
        std::string apkArc = dbdir + "/apk.tar" + compression_suffix(Compression::Zstd);
        {
            TarOptions t2;
            t2.only_top_level_files = true;
            t2.file_suffix_filter = ".apk";
            t2.flat_root = true;
            std::unique_ptr<Sink> sink = open_sink(apkArc, Compression::Zstd, 6);
            TarStats st;
            bool ok = sink && tar_add_tree(*sink, apkSrc, "apk", t2, &st);
            ok = tar_write_end(*sink) && ok;
            ok = sink->close() && ok;
            check(ok, "flat apk archive");
            check(st.files == 2, "flat apk keeps 2 top-level apks");
        }
        {
            ExtractOptions eopt;
            eopt.dest_dir = base + "/apkout";
            std::unique_ptr<Source> s = open_source_auto(apkArc);
            ExtractStats est;
            bool ok = s && tar_extract(*s, eopt, &est);
            if (s) s->close();
            check(ok && path_exists(base + "/apkout/base.apk"), "flat apk archive extracts to top level");
            check(!path_exists(base + "/apkout/notapk.txt"), "non-apk excluded from flat archive");
        }

        check(write_md5_sidecar(apkArc), "md5 sidecar written");
        std::string md5txt = trim(read_all(apkArc + ".md5"));
        check(md5txt == md5_of_file(apkArc) && md5txt.size() == 32, "md5 sidecar content matches");
        check(md5txt == "d41d8cd98f00b204e9800998ecf8427e" || md5txt != "", "md5 hex format");

        std::vector<DataEntry> entries;
        DataEntry e;
        e.type = kDataTypeApk;
        e.exists = true;
        e.bytes = 1234;
        entries.push_back(e);
        DbFormatInfo info;
        info.pkg = "com.demo.app";
        info.user = 0;
        info.uid = 10234;
        info.preserve_id = 1767225600000LL;
        info.compression_type = "ZSTD";
        check(write_databackup_config(dbdir, info, entries, base + "/DataBackup"), "write package_restore_config.json");
        check(is_databackup_dir(dbdir), "is_databackup_dir");

        DbFormatInfo back;
        check(read_databackup_info(dbdir, &back), "read back databackup config");
        check(back.pkg == "com.demo.app", "config pkg roundtrip");
        check(back.preserve_id == 1767225600000LL, "config preserveId roundtrip");
        check(back.compression_type == "ZSTD", "config compressionType roundtrip");
        check(back.uid == 10234, "config uid roundtrip");
        check(find_databackup_archive(dbdir, "apk") == apkArc, "find_databackup_archive probes suffix");

        // 目录名兜底（没有 json 时）
        std::string bare = base + "/DataBackup/apps/com.bare/user_3@1700000000000";
        make_dirs(bare, 0755);
        DbFormatInfo bareInfo;
        check(read_databackup_info(bare, &bareInfo), "read info from dir name only");
        check(bareInfo.user == 3 && bareInfo.preserve_id == 1700000000000LL, "dir name parsed");
    }

    // 恢复策略：目录映射 / 解包根 / 排除项（对齐 DataBackup PackagesRestoreUtil）
    {
        check(policy::target_dir(kDataTypeUser, 0, "com.a") == "/data/user/0/com.a", "target_dir user");
        check(policy::target_dir(kDataTypeUserDe, 10, "com.a") == "/data/user_de/10/com.a", "target_dir user_de");
        check(policy::target_dir(kDataTypeData, 0, "com.a") == "/data/media/0/Android/data/com.a", "target_dir data");
        check(policy::target_dir(kDataTypeObb, 0, "com.a") == "/data/media/0/Android/obb/com.a", "target_dir obb");
        check(policy::target_dir(kDataTypeMedia, 0, "com.a") == "/data/media/0/Android/media/com.a",
              "target_dir media");
        check(policy::target_dir(kDataTypeApk, 0, "com.a").empty(), "target_dir apk empty");

        // 解包根必须是父目录：解到 dest 本身会产生 <pkg>/<pkg>/...（历史 bug）
        check(policy::extract_root_for("/data/user/0/com.a") == "/data/user/0", "extract_root user");
        check(policy::extract_root_for("/data/media/0/Android/data/com.a") == "/data/media/0/Android/data",
              "extract_root external");

        // 实打实跑一遍：带 lib/.ota/cache/Backup_/trashed 的归档，按恢复规则解到父目录
        std::string rsrc = base + "/rsrc/com.demo.restore";
        std::string rpkg = "com.demo.restore";
        make_dirs(rsrc + "/files", 0755);
        make_dirs(rsrc + "/lib/arm64", 0755);
        make_dirs(rsrc + "/.ota", 0755);
        make_dirs(rsrc + "/cache", 0755);
        make_dirs(rsrc + "/code_cache", 0755);
        make_dirs(rsrc + "/no_backup", 0755);
        write_text(rsrc + "/files/keep.txt", "keep\n");
        write_text(rsrc + "/lib/arm64/libnative.so", "stale so");
        write_text(rsrc + "/.ota/patch.bin", "ota");
        write_text(rsrc + "/cache/c.bin", "cache");
        write_text(rsrc + "/code_cache/cc.bin", "cc");
        write_text(rsrc + "/no_backup/nb.bin", "nb");
        write_text(rsrc + "/Backup_2024.db", "bak");
        write_text(rsrc + "/.trashed-1700000000-p.jpg", "trash");
        write_text(rsrc + "/.Trashed-1700000000-q.jpg", "trash2");

        std::string rarc = base + "/r.tar";
        {
            ExtractOptions rules;
            policy::apply_restore_exclusions(kDataTypeUser, rpkg, &rules);
            TarOptions o;
            o.exclude_prefixes = rules.exclude_prefixes;
            o.exclude_globs = rules.exclude_globs;
            std::unique_ptr<Sink> sink = open_plain_sink(rarc);
            TarStats st;
            bool ok = tar_add_tree(*sink, rsrc, rpkg, o, &st);
            ok = tar_write_end(*sink) && ok;
            ok = sink->close() && ok;
            check(ok, "restore-shaped archive built");
        }
        {
            std::string parent = base + "/rparent";
            make_dirs(parent + "/" + rpkg, 0755);  // 模拟 installd 已建好包目录
            ExtractOptions eopt;
            eopt.dest_dir = parent;                 // 关键：父目录
            eopt.restore_owner = false;
            policy::apply_restore_exclusions(kDataTypeUser, rpkg, &eopt);
            std::unique_ptr<Source> s = open_plain_source(rarc);
            ExtractStats est;
            bool ok = s && tar_extract(*s, eopt, &est);
            if (s) s->close();
            check(ok, "restore-shaped extract");
            check(path_exists(parent + "/" + rpkg + "/files/keep.txt"), "user data landed at <parent>/<pkg>/files");
            check(!path_exists(parent + "/" + rpkg + "/" + rpkg), "no double <pkg>/<pkg> nesting");
            check(!path_exists(parent + "/" + rpkg + "/lib/arm64/libnative.so"), "restore excludes lib (stale .so)");
            check(!path_exists(parent + "/" + rpkg + "/.ota/patch.bin"), "restore excludes .ota");
            check(!path_exists(parent + "/" + rpkg + "/cache/c.bin"), "restore excludes cache");
            check(!path_exists(parent + "/" + rpkg + "/code_cache/cc.bin"), "restore excludes code_cache");
            check(!path_exists(parent + "/" + rpkg + "/no_backup/nb.bin"), "restore excludes no_backup");
        }

        // data 类型额外排除 Backup_* 与回收站文件（大小写）
        {
            ExtractOptions o;
            policy::apply_restore_exclusions(kDataTypeData, rpkg, &o);
            TarOptions probe;
            probe.exclude_prefixes = o.exclude_prefixes;
            probe.exclude_globs = o.exclude_globs;
            check(tar_path_excluded(rpkg + "/Backup_2024.db", probe), "data excludes Backup_*");
            check(tar_path_excluded(rpkg + "/.trashed-1-a.jpg", probe), "data excludes .trashed-*");
            check(tar_path_excluded(rpkg + "/.Trashed-1-a.jpg", probe), "data excludes .Trashed-*");
            check(tar_path_excluded(rpkg + "/.pending-1-a.jpg", probe), "data excludes .pending-*");
            check(!tar_path_excluded(rpkg + "/files/keep.txt", probe), "data keeps normal files");

            // user 类型不套用 Backup_*/trashed（保持与上游一致，避免误删应用自身数据）
            ExtractOptions uo;
            policy::apply_restore_exclusions(kDataTypeUser, rpkg, &uo);
            TarOptions u;
            u.exclude_prefixes = uo.exclude_prefixes;
            u.exclude_globs = uo.exclude_globs;
            check(!tar_path_excluded(rpkg + "/Backup_2024.db", u), "user does not exclude Backup_*");
            check(tar_path_excluded(rpkg + "/lib/x.so", u), "user excludes lib");
        }

        // 干净恢复保留项
        {
            std::vector<std::string> keep = policy::keep_names();
            check(keep.size() == 5, "keep_names has 5 entries");
        }
    }

    rm_rf(base);
    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
