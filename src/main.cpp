// appbackup —— 应用数据打包 / 恢复工具（需 root）
//
//   appbackup backup  --pkg <pkg> [--user 0] [--types user,user_de,data,obb,media,apk]
//                       [--out DIR] [--compress gzip|none] [--kill none|am|kill]
//   appbackup restore --in DIR [--pkg <pkg>] [--user 0] [--types ...]
//                       [--clean] [--install-apk] [--no-selinux] [--no-verify]
//                       [--no-checksum] [--force-checksum]
//   appbackup list    --in DIR
//   appbackup info    --pkg <pkg> [--user 0]
//   appbackup toolchain   打印实际使用的 tar / zstd 路径与能力探测结果（排障用）

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "backup.h"
#include "common.h"
#include "meta.h"
#include "toolchain.h"
#include "restore.h"
#include "sink.h"

using namespace appbackup;

namespace {

void print_usage() {
    printf(
        "%s %s - Android app data backup / restore (root required)\n"
        "\n"
        "Usage:\n"
        "  %s backup  --pkg <pkg> [options]\n"
        "  %s restore --in <backup dir> [options]\n"
        "  %s list    --in <backup dir>\n"
        "  %s info    --pkg <pkg> [--user N]\n"
        "  %s toolchain          show tar / zstd paths and supported options\n"
        "\n"
        "backup options:\n"
        "  --pkg <name>        package name to back up (required)\n"
        "  --user <n>          user id, default 0\n"
        "  --types <list>      comma separated, default all: user,user_de,data,obb,media,apk\n"
        "  --out <dir>         backup root, default cwd; creates apps/<label>_<pkg>/user_<uid>\n"
        "  --compress <mode>   zstd (default) / none; DataBackup does not support gzip\n"
        "  --level <n>         zstd level 1-19, default 1 (DataBackup default)\n"
        "  --threads <n>       .manifest threads=, default 2\n"
        "  --preserve          archive existing main backup as user_<uid>@<timestamp>\n"
        "  --no-incremental    ignore .manifest fingerprint, force full repack\n"
        "  --label <value>     app dir prefix: auto (default, app name) / plain (pkg only) / custom\n"
        "  --kill <mode>       stop app before packing: none (default) / am / kill\n"
        "  --no-permissions    set permissionState=Disabled in config\n"
        "  --no-ssaid          set ssaidState=Disabled in config\n"
        "\n"
        "restore options:\n"
        "  --in <dir>          version dir / app dir / backup root (auto-descends to newest)\n"
        "  --pkg <name>        override package name from meta.json\n"
        "  --user <n>          user id, default from meta.json\n"
        "  --types <list>      restore only these types\n"
        "  --clean             wipe target first level (keeps lib/cache/code_cache/no_backup/.ota)\n"
        "  --install-apk       reinstall APK even if already installed\n"
        "  --no-apk            do not install APK (cannot restore data if app is uninstalled)\n"
        "  --no-selinux        skip SELinux relabel\n"
        "  --no-permissions    skip runtime permissions / AppOps restore\n"
        "  --no-ssaid          skip SSAID restore\n"
        "  --no-verify         skip ownership check\n"
        "  --no-checksum       skip <archive>.md5 check before restore\n"
        "  --force-checksum    continue even if MD5 mismatches\n"
        "\n"
        "Global options:\n"
        "  -v, --verbose       verbose logging\n"
        "  -q, --quiet         errors only\n"
        "  -h, --help          show this help\n"
        "  --version           show version\n"
        "\n"
        "Data dirs:\n"
        "  user     /data/user/<N>/<pkg>\n"
        "  user_de  /data/user_de/<N>/<pkg>\n"
        "  data     /data/media/<N>/Android/data/<pkg>\n"
        "  obb      /data/media/<N>/Android/obb/<pkg>\n"
        "  media    /data/media/<N>/Android/media/<pkg>\n"
        "  apk      all *.apk in the dir of pm path\n"
        "\n"
        "Examples:\n"
        "  %s backup --pkg com.tencent.mm --out /sdcard/DataBackup --kill am\n"
        "  %s backup --pkg com.tencent.mm --out /sdcard/DataBackup --preserve --level 3\n"
        "  %s restore --in /sdcard/DataBackup/apps/WeChat_com.tencent.mm/user_0 --install-apk --clean\n"
        "  %s restore --in /sdcard/DataBackup --pkg com.tencent.mm --clean\n",
        kToolName, kToolVersion, kToolName, kToolName, kToolName, kToolName, kToolName, kToolName, kToolName,
        kToolName, kToolName);
}

std::vector<std::string> parse_type_list(const std::string& raw) {
    std::vector<std::string> out;
    for (auto& t : split(raw, ',')) {
        std::string v = trim(t);
        if (!v.empty()) out.push_back(v);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }

    std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        print_usage();
        return 0;
    }
    if (cmd == "--version" || cmd == "version") {
        printf("%s %s\n", kToolName, kToolVersion);
        return 0;
    }

    BackupOptions bopt;
    RestoreOptions ropt;
    std::string list_dir;

    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for %s\n", name);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--pkg") {
            bopt.pkg = next("--pkg");
            ropt.pkg = bopt.pkg;
        } else if (a == "--user") {
            int u = atoi(next("--user").c_str());
            bopt.user = u;
            ropt.user = u;
        } else if (a == "--types") {
            std::vector<std::string> t = parse_type_list(next("--types"));
            bopt.types = t;
            ropt.types = t;
        } else if (a == "--out") {
            bopt.out_dir = next("--out");
        } else if (a == "--compress") {
            Compression c;
            if (!parse_compression(next("--compress"), &c)) {
                fprintf(stderr, "unknown compression mode (zstd / gzip / none)\n");
                return 2;
            }
            bopt.compression = c;
        } else if (a == "--level") {
            bopt.level = atoi(next("--level").c_str());
            if (bopt.level < 1) bopt.level = 1;
            if (bopt.level > 19) bopt.level = 19;
        } else if (a == "--preserve") {
            bopt.preserve = true;
        } else if (a == "--no-incremental") {
            bopt.incremental = false;
        } else if (a == "--threads") {
            bopt.threads = atoi(next("--threads").c_str());
            if (bopt.threads < 0) bopt.threads = 0;
            if (bopt.threads > 256) bopt.threads = 256;
        } else if (a == "--label") {
            // 默认 auto：apps/<应用名>_<pkg>/user_N；plain 退回 apps/<pkg>/user_N
            bopt.label = next("--label");
        } else if (a == "--format") {
            // 只有 DataBackup 一种输出格式，--format 仅作旧脚本兼容占位，忽略具体取值
            std::string f = next("--format");
            if (f != "databackup" && f != "db") {
                log_warn("--format %s ignored: tool writes DataBackup layout only (apps/<label>_<pkg>/user_<uid>/)", f.c_str());
            }
        } else if (a == "--kill") {
            std::string m = next("--kill");
            if (m == "none") bopt.kill = KillMode::None;
            else if (m == "am") bopt.kill = KillMode::Am;
            else if (m == "kill") bopt.kill = KillMode::Kill;
            else {
                fprintf(stderr, "unknown --kill mode: %s\n", m.c_str());
                return 2;
            }
        } else if (a == "--in") {
            ropt.in_dir = next("--in");
            list_dir = ropt.in_dir;
        } else if (a == "--clean") {
            ropt.clean = true;
        } else if (a == "--install-apk") {
            // 兼容旧参数：强制覆盖安装（默认仅在应用未安装时自动安装）
            ropt.install_apk = true;
            ropt.force_reinstall = true;
        } else if (a == "--no-apk") {
            // 不安装 APK（应用已卸载时会失败，与上游只恢复数据时的行为一致）
            ropt.install_apk = false;
            ropt.force_reinstall = false;
        } else if (a == "--no-selinux") {
            ropt.do_selinux = false;
        } else if (a == "--no-verify") {
            ropt.verify_owner = false;
        } else if (a == "--no-permissions") {
            ropt.restore_permissions = false;
            bopt.backup_permissions = false;
        } else if (a == "--no-ssaid") {
            ropt.restore_ssaid = false;
            bopt.backup_ssaid = false;
        } else if (a == "--no-checksum") {
            ropt.verify_checksum = false;
        } else if (a == "--force-checksum") {
            ropt.force_checksum = true;
        } else if (a == "-v" || a == "--verbose") {
            g_log_level = 2;
        } else if (a == "-q" || a == "--quiet") {
            g_log_level = 0;
        } else if (a == "-h" || a == "--help") {
            print_usage();
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", a.c_str());
            return 2;
        }
    }

    if (cmd == "backup") return run_backup(bopt);
    if (cmd == "restore") return run_restore(ropt);
    if (cmd == "list") {
        if (list_dir.empty()) {
            fprintf(stderr, "list requires --in <backup dir>\n");
            return 2;
        }
        return run_list(list_dir, bopt.pkg);
    }
    if (cmd == "toolchain") {
        // 排障入口：确认走的是内嵌 GNU tar 还是系统 toybox tar，以及哪些选项会降级
        printf("%s\n", toolchain::describe().c_str());
        return 0;
    }
    if (cmd == "info") {
        if (bopt.pkg.empty()) {
            fprintf(stderr, "info requires --pkg <pkg>\n");
            return 2;
        }
        return run_info(bopt.pkg, bopt.user);
    }

    fprintf(stderr, "unknown subcommand: %s\n", cmd.c_str());
    print_usage();
    return 2;
}
