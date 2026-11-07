// 主机侧自测：调用系统 tar/zstd 的打包/解包往返、排除项、APK 扁平、确定性、互操作
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common.h"
#include "dbformat.h"
#include "fsops.h"
#include "fingerprint.h"
#include "manifest.h"
#include "md5.h"
#include "meta.h"
#include "proc.h"
#include "restore_policy.h"
#include "sink.h"
#include "sysarchive.h"

using namespace appbackup;

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
    std::string base = "/tmp/appbackup_rt_" + std::to_string(getpid());

    // 备份产物：清单指纹 / manifest 格式 / package_restore_config.json（对齐 DataBackup）
    {
        std::string bsrc = base + "/bsrc/com.demo.bk";
        make_dirs(bsrc + "/files", 0755);
        make_dirs(bsrc + "/cache", 0755);
        write_text(bsrc + "/files/a.txt", "x\n");
        write_text(bsrc + "/cache/c.bin", "c");

        HashOptions hopt;
        hopt.prune_names = {".ota", "cache", "lib", "code_cache", "no_backup"};
        std::string h = compute_source_hash(bsrc, hopt);
        check(h.size() == 64, "source hash length");

        std::string prune;
        for (const char* n : {".ota", "cache", "lib", "code_cache", "no_backup"}) {
            if (!prune.empty()) prune += " -o ";
            prune += "-path '" + bsrc + "/" + n + "'";
        }
        ExecResult r = run_shell("find '" + bsrc + "' \\( " + prune +
                                 " \\) -prune -o -type f -printf '%P|%s|%T@\\n' | LC_ALL=C sort | sha256sum");
        if (r.code == 0 && trim(r.out).size() >= 64) {
            check(h == trim(r.out).substr(0, 64), "hash matches find -printf | sort | sha256sum");
        } else {
            printf("[SKIP] find unavailable, hash cross-check skipped\n");
        }
        check(compute_source_hash(bsrc + "/nope", hopt).empty(), "hash fails on missing dir");

        ManifestParams mp;
        mp.ct = "ZSTD";
        mp.level = 1;
        mp.threads = 1;
        mp.follow = false;
        mp.excl = {"com.demo.bk/cache", "com.demo.bk/lib"};
        std::string text = manifest_serialize(h, mp);
        std::string want = "v=1\nhash=" + h +
                           "\nct=ZSTD\nlevel=1\nthreads=1\nfollow=false\n"
                           "excl=com.demo.bk/cache|com.demo.bk/lib\n";
        check(text == want, "manifest byte format matches upstream");
        std::string h2;
        ManifestParams mp2;
        check(manifest_parse(text, &h2, &mp2) && h2 == h && manifest_params_equal(mp, mp2),
              "manifest parse roundtrip");

        std::string madir = base + "/man";
        make_dirs(madir, 0755);
        std::string marc = madir + "/user.tar.zst";
        write_text(marc, "dummy");
        check(manifest_write(marc, h, mp), "manifest sidecar written");
        check(manifest_read(marc, &h2, &mp2) && h2 == h, "manifest sidecar read back");

        std::vector<DataEntry> es;
        DataEntry eu;
        eu.type = kDataTypeUser;
        eu.exists = true;
        eu.bytes = 42;
        es.push_back(eu);
        std::string dir2 = base + "/db2/apps/com.demo.bk/user_0";
        make_dirs(dir2, 0755);
        DbFormatInfo inf;
        inf.pkg = "com.demo.bk";
        inf.user = 0;
        inf.uid = 10234;
        inf.compression_type = "ZSTD";
        check(write_databackup_config(dir2, inf, es, base + "/db2"), "config written for main backup");
        std::string cfg = read_all(dir2 + "/package_restore_config.json");
        check(cfg.find("\"opType\": \"RESTORE\"") != std::string::npos, "config opType RESTORE");
        check(cfg.find("\"preserveId\": 0") != std::string::npos, "config preserveId 0 on main dir");
        check(cfg.find("\"userState\": \"Selected\"") != std::string::npos, "config userState Selected");
        check(cfg.find("\"dataState\": \"Disabled\"") != std::string::npos, "config missing type Disabled");
        check(cfg.find("\"lastBackupTime\": 0") == std::string::npos, "config lastBackupTime stamped");
        check(cfg.find("\"userBytes\": 42") != std::string::npos, "config userBytes from archive size");
        check(cfg.find("\"compressionType\": \"ZSTD\"") != std::string::npos, "config compressionType");
    }

    rm_rf(base);

    // ---- sys_backup / sys_restore 往返（调用系统 tar，与上游 TarUtil 指令一致）----
    {
        std::string src = base + "/src/com.demo.app";
        make_dirs(src + "/sub/deep", 0755);
        write_text(src + "/a.txt", "hello appbackup\n");
        write_text(src + "/sub/b.bin", std::string(5000, 'x'));
        write_text(src + "/empty.dat", "");
        write_text(src + "/cache/should_skip.txt", "cache");
        write_text(src + "/Backup_2024.db", "skip me");
        std::string long_dir = src + "/" + std::string(60, 'd') + "/" + std::string(60, 'e');
        make_dirs(long_dir, 0755);
        write_text(long_dir + "/" + std::string(90, 'f') + ".txt", "long path ok\n");
        if (symlink("a.txt", (src + "/link_to_a").c_str()) != 0) perror("symlink");
        if (link((src + "/a.txt").c_str(), (src + "/hard_to_a").c_str()) != 0) perror("link");

        std::string tar_path = base + "/t.tar";
        SysBackupOpt bo;
        bo.srcDir = base + "/src";
        bo.src = "com.demo.app";
        bo.dst = tar_path;
        bo.compression = Compression::Plain;
        bo.excludes = {"com.demo.app/cache", "Backup_*"};
        uint64_t unc = 0;
        check(sys_backup(bo, &unc) && path_exists(tar_path), "sys_backup plain (data)");

        // 系统 tar 能读我们的产物
        check(run_shell("tar -tf '" + tar_path + "' >/dev/null 2>&1").code == 0, "system tar can list our archive");
        check(trim(run_shell("tar -tf '" + tar_path + "' | grep -c 'cache\\|Backup_'").out) == "0",
              "exclusions honoured (no cache/Backup_ entries)");

        // 解包往返
        std::string out = base + "/out";
        SysRestoreOpt ro;
        ro.src = tar_path;
        ro.dstDir = out;
        ro.excludes = {"com.demo.app/cache", "Backup_*"};
        check(sys_restore(ro), "sys_restore plain (data)");
        check(read_all(out + "/com.demo.app/a.txt") == "hello appbackup\n", "content roundtrip");
        check(read_all(out + "/com.demo.app/sub/b.bin").size() == 5000, "binary size roundtrip");
        check(read_all(out + "/com.demo.app/empty.dat").empty(), "empty file");
        check(path_exists(out + "/com.demo.app/" + std::string(60, 'd') + "/" + std::string(60, 'e') + "/" +
                           std::string(90, 'f') + ".txt"),
              "long path (>255 bytes) roundtrip");
        struct stat st;
        check(lstat((out + "/com.demo.app/link_to_a").c_str(), &st) == 0 && S_ISLNK(st.st_mode),
              "symlink restored");
        check(lstat((out + "/com.demo.app/hard_to_a").c_str(), &st) == 0 && st.st_nlink == 2,
              "hardlink restored");
        check(!path_exists(out + "/com.demo.app/cache/should_skip.txt"), "excluded dir not extracted");
    }

    // ---- APK 扁平打包：系统 tar 用 ./*.apk glob，天然不含 lib/oat 子目录 ----
    {
        std::string apksrc = base + "/apksrc";
        make_dirs(apksrc + "/lib/arm64", 0755);
        make_dirs(apksrc + "/oat/arm64", 0755);
        write_text(apksrc + "/lib/arm64/libnative.so", "so");
        write_text(apksrc + "/oat/arm64/base.odex", "odex");
        write_text(apksrc + "/base.apk", "PK\x03\x04 base");
        write_text(apksrc + "/split_config.arm64_v8a.apk", "PK\x03\x04 split");
        write_text(apksrc + "/notapk.txt", "skip");

        std::string apkArc = base + "/apk.tar";
        SysBackupOpt bo;
        bo.apk_mode = true;
        bo.srcDir = apksrc;
        bo.src = "./*.apk";
        bo.dst = apkArc;
        bo.compression = Compression::Plain;
        check(sys_backup(bo) && path_exists(apkArc), "apk pack (cd + ./*.apk glob)");

        std::string listing = run_shell("tar -tf '" + apkArc + "'").out;
        check(listing.find("lib") == std::string::npos && listing.find("oat") == std::string::npos &&
                  listing.find("notapk") == std::string::npos,
              "apk archive contains only ./*.apk (no lib/oat/notapk)");
        check(trim(run_shell("tar -tf '" + apkArc + "' | grep -c '\\.apk'").out) == "2",
              "apk archive has exactly 2 top-level apks");

        std::string apkout = base + "/apkout";
        SysRestoreOpt ro;
        ro.src = apkArc;
        ro.dstDir = apkout;
        check(sys_restore(ro) && path_exists(apkout + "/base.apk") &&
                  path_exists(apkout + "/split_config.arm64_v8a.apk"),
              "apk extracts to top-level");
        check(!path_exists(apkout + "/notapk.txt"), "non-apk excluded from apk archive");
    }

    // ---- 确定性：同目录打包两次，系统 tar 字节一致（同一设备 readdir 顺序稳定）----
    {
        std::string detsrc = base + "/detsrc/com.demo.det";
        make_dirs(detsrc + "/a", 0755);
        write_text(detsrc + "/a/z.txt", "z");
        write_text(detsrc + "/b.txt", "b");
        auto pack = [&](const std::string& p) -> std::string {
            SysBackupOpt bo;
            bo.srcDir = base + "/detsrc";
            bo.src = "com.demo.det";
            bo.dst = p;
            bo.compression = Compression::Plain;
            sys_backup(bo);
            return md5_of_file(p);
        };
        std::string m1 = pack(base + "/det1.tar");
        std::string m2 = pack(base + "/det2.tar");
        check(m1 == m2 && m1.size() == 32, "deterministic: same tree packs to identical tar bytes");
    }

    // ---- DataBackup 兼容结构：package_restore_config.json + md5 + 目录命名 ----
    {
        std::string dbdir = base + "/DataBackup/apps/com.demo.app/user_0@1767225600000";
        make_dirs(dbdir, 0755);
        // apk 归档用系统 tar 扁平打包
        std::string apksrc = base + "/apksrc2";
        make_dirs(apksrc, 0755);
        write_text(apksrc + "/base.apk", "PK\x03\x04 fake apk");
        write_text(apksrc + "/split_config.arm64_v8a.apk", "PK\x03\x04 split");
        write_text(apksrc + "/notapk.txt", "skip");
        std::string apkArc = dbdir + "/apk.tar" + compression_suffix(Compression::Plain);
        SysBackupOpt bo;
        bo.apk_mode = true;
        bo.srcDir = apksrc;
        bo.src = "./*.apk";
        bo.dst = apkArc;
        bo.compression = Compression::Plain;
        check(sys_backup(bo), "flat apk archive (databackup layout)");

        check(write_md5_sidecar(apkArc), "md5 sidecar written");
        std::string md5txt = trim(read_all(apkArc + ".md5"));
        check(md5txt == md5_of_file(apkArc) && md5txt.size() == 32, "md5 sidecar content matches");

        std::vector<DataEntry> entries;
        DataEntry e;
        e.type = kDataTypeApk;
        e.exists = true;
        e.bytes = static_cast<uint64_t>(file_size(apkArc));
        entries.push_back(e);
        DbFormatInfo info;
        info.pkg = "com.demo.app";
        info.user = 0;
        info.uid = 10234;
        info.preserve_id = 1767225600000LL;
        info.compression_type = "ZSTD";
        check(write_databackup_config(dbdir, info, entries, base + "/DataBackup"),
              "write package_restore_config.json");
        check(is_databackup_dir(dbdir), "is_databackup_dir");

        DbFormatInfo back;
        check(read_databackup_info(dbdir, &back), "read back databackup config");
        check(back.pkg == "com.demo.app", "config pkg roundtrip");
        check(back.preserve_id == 1767225600000LL, "config preserveId roundtrip");
        check(back.compression_type == "ZSTD", "config compressionType roundtrip");
        check(back.uid == 10234, "config uid roundtrip");
        check(find_databackup_archive(dbdir, "apk") == apkArc, "find_databackup_archive probes suffix");

        std::string bare = base + "/DataBackup/apps/com.bare/user_3@1700000000000";
        make_dirs(bare, 0755);
        DbFormatInfo bareInfo;
        check(read_databackup_info(bare, &bareInfo), "read info from dir name only");
        check(bareInfo.user == 3 && bareInfo.preserve_id == 1700000000000LL, "dir name parsed");
    }

    // ---- 恢复策略：目录映射 / 解包根 / 排除项（对齐 DataBackup PackagesRestoreUtil）----
    {
        check(policy::target_dir(kDataTypeUser, 0, "com.a") == "/data/user/0/com.a", "target_dir user");
        check(policy::target_dir(kDataTypeUserDe, 10, "com.a") == "/data/user_de/10/com.a",
              "target_dir user_de");
        check(policy::target_dir(kDataTypeData, 0, "com.a") == "/data/media/0/Android/data/com.a",
              "target_dir data");
        check(policy::target_dir(kDataTypeObb, 0, "com.a") == "/data/media/0/Android/obb/com.a",
              "target_dir obb");
        check(policy::target_dir(kDataTypeMedia, 0, "com.a") == "/data/media/0/Android/media/com.a",
              "target_dir media");
        check(policy::target_dir(kDataTypeApk, 0, "com.a").empty(), "target_dir apk empty");

        // 解包根必须是父目录
        check(policy::extract_root_for("/data/user/0/com.a") == "/data/user/0", "extract_root user");
        check(policy::extract_root_for("/data/media/0/Android/data/com.a") == "/data/media/0/Android/data",
              "extract_root external");

        // 实打实跑：含 lib/.ota/cache/Backup_/trashed 的归档，按恢复规则解到父目录并排除
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

        // 不排除地打包（保留全部），再在解包侧排除，验证 --exclude 在恢复时生效
        std::string rarc = base + "/r.tar";
        {
            SysBackupOpt bo;
            bo.srcDir = base + "/rsrc";
            bo.src = rpkg;
            bo.dst = rarc;
            bo.compression = Compression::Plain;
            check(sys_backup(bo) && path_exists(rarc), "restore-shaped archive built (no exclude)");
        }
        {
            std::string parent = base + "/rparent";
            make_dirs(parent + "/" + rpkg, 0755);  // 模拟 installd 已建好包目录
            SysRestoreOpt ro;
            ro.src = rarc;
            ro.dstDir = parent;  // 关键：父目录
            policy::apply_restore_exclusions(kDataTypeUser, rpkg, &ro.excludes);
            check(sys_restore(ro), "restore-shaped extract");
            check(path_exists(parent + "/" + rpkg + "/files/keep.txt"),
                  "user data landed at <parent>/<pkg>/files");
            check(!path_exists(parent + "/" + rpkg + "/" + rpkg), "no double <pkg>/<pkg> nesting");
            check(!path_exists(parent + "/" + rpkg + "/lib/arm64/libnative.so"),
                  "restore excludes lib (stale .so)");
            check(!path_exists(parent + "/" + rpkg + "/.ota/patch.bin"), "restore excludes .ota");
            check(!path_exists(parent + "/" + rpkg + "/cache/c.bin"), "restore excludes cache");
            check(!path_exists(parent + "/" + rpkg + "/code_cache/cc.bin"), "restore excludes code_cache");
            check(!path_exists(parent + "/" + rpkg + "/no_backup/nb.bin"), "restore excludes no_backup");
        }

        // data 类型额外排除 Backup_*；user 类型不套用 Backup_*（保持与上游一致）
        std::vector<std::string> dataExcl, userExcl;
        policy::apply_restore_exclusions(kDataTypeData, rpkg, &dataExcl);
        policy::apply_restore_exclusions(kDataTypeUser, rpkg, &userExcl);
        bool dataHasBackup = false, userHasBackup = false;
        for (const auto& s : dataExcl) {
            if (s == "Backup_*") dataHasBackup = true;
        }
        for (const auto& s : userExcl) {
            if (s == "Backup_*") userHasBackup = true;
        }
        check(dataHasBackup, "data excludes Backup_*");
        check(!userHasBackup, "user does not exclude Backup_*");
        check(userExcl.size() == 5, "user has 5 fixed excludes");

        std::vector<std::string> keep = policy::keep_names();
        check(keep.size() == 5, "keep_names has 5 entries");
    }

    // ---- zstd 往返（仅当主机有 zstd 二进制；设备上必有）----
    {
        ExecResult has = run_shell("command -v zstd >/dev/null 2>&1");
        if (has.code == 0) {
            std::string zsrc = base + "/zsrc/com.demo.z";
            make_dirs(zsrc, 0755);
            write_text(zsrc + "/a.txt", "zstd content\n");
            write_text(zsrc + "/b.bin", std::string(2000, 'y'));
            std::string zst = base + "/z.tar.zst";
            SysBackupOpt bo;
            bo.srcDir = base + "/zsrc";
            bo.src = "com.demo.z";
            bo.dst = zst;
            bo.compression = Compression::Zstd;
            bo.level = 3;
            check(sys_backup(bo) && file_is_zstd(zst), "sys_backup zstd");
            check(detect_compression(zst) == Compression::Zstd, "detect_compression -> zstd");

            std::string zout = base + "/zout";
            SysRestoreOpt ro;
            ro.src = zst;
            ro.dstDir = zout;
            check(sys_restore(ro) && read_all(zout + "/com.demo.z/a.txt") == "zstd content\n",
                  "zstd content roundtrip");
            check(run_shell("zstd -d -c '" + zst + "' 2>/dev/null | tar -tf - >/dev/null 2>&1").code == 0,
                  "system zstd+tar can read our .tar.zst");
        } else {
            printf("[SKIP] system zstd CLI not present on host (device has it)\n");
        }
    }

    rm_rf(base);
    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
