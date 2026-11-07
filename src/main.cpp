// appbackup —— 应用数据打包 / 恢复工具（需 root）
//
//   appbackup backup  --pkg <pkg> [--user 0] [--types user,user_de,data,obb,media,apk]
//                       [--out DIR] [--compress gzip|none] [--kill none|am|kill]
//   appbackup restore --in DIR [--pkg <pkg>] [--user 0] [--types ...]
//                       [--clean] [--install-apk] [--no-selinux] [--no-verify]
//                       [--no-checksum] [--force-checksum]
//   appbackup list    --in DIR
//   appbackup info    --pkg <pkg> [--user 0]

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "backup.h"
#include "common.h"
#include "meta.h"
#include "restore.h"
#include "sink.h"

using namespace appbackup;

namespace {

void print_usage() {
    printf(
        "%s %s —— 应用数据打包 / 恢复（Android, 需 root）\n"
        "\n"
        "用法:\n"
        "  %s backup  --pkg <包名> [选项]\n"
        "  %s restore --in <备份目录> [选项]\n"
        "  %s list    --in <备份目录>\n"
        "  %s info    --pkg <包名> [--user N]\n"
        "\n"
        "backup 选项:\n"
        "  --pkg <name>        要打包的应用包名（必填）\n"
        "  --user <n>          用户 id，默认 0\n"
        "  --types <list>      逗号分隔，默认全部：user,user_de,data,obb,media,apk\n"
        "  --out <dir>         备份根目录，默认当前目录；会在其下建 apps/<包名>/user_<uid>\n"
        "  --compress <mode>   zstd（默认）/ none；DataBackup 不支持 gzip\n"
        "  --level <n>         zstd 压缩等级 1-19，默认 1（与 DataBackup 默认一致）\n"
        "  --preserve          已有主备份时先归档为 user_<uid>@<时间戳>\n"
        "  --no-incremental    不读 .manifest 指纹，强制全量重打\n"
        "  --kill <mode>       打包前停进程：none（默认）/ am / kill\n"
        "\n"
        "restore 选项:\n"
        "  --in <dir>          备份目录：版本目录 / 应用目录 / 备份根目录均可，自动下探到最新版本\n"
        "  --pkg <name>        覆盖 meta.json 里的包名（从根目录下探时用来选应用）\n"
        "  --user <n>          用户 id，默认沿用 meta.json\n"
        "  --types <list>      只恢复指定类型\n"
        "  --clean             干净恢复：先清空目标第一层（保留 lib/cache/code_cache/no_backup/.ota）\n"
        "  --install-apk       先安装备份里的 APK（多 APK 自动走 install-create 会话），再恢复数据\n"
        "  --no-selinux        跳过 SELinux 重打标签\n"
        "  --no-verify         跳过属主校验\n"
        "  --no-checksum       跳过恢复前的 <归档>.md5 校验\n"
        "  --force-checksum    MD5 不一致时仍强制恢复\n"
        "\n"
        "全局选项:\n"
        "  -v, --verbose       输出调试日志\n"
        "  -q, --quiet         只输出错误\n"
        "  -h, --help          显示本帮助\n"
        "  --version           显示版本\n"
        "\n"
        "数据目录:\n"
        "  user     /data/user/<N>/<pkg>\n"
        "  user_de  /data/user_de/<N>/<pkg>\n"
        "  data     /data/media/<N>/Android/data/<pkg>\n"
        "  obb      /data/media/<N>/Android/obb/<pkg>\n"
        "  media    /data/media/<N>/Android/media/<pkg>\n"
        "  apk      pm path 的父目录下所有 *.apk\n"
        "\n"
        "示例:\n"
        "  %s backup --pkg com.tencent.mm --out /sdcard/DataBackup --kill am\n"
        "  %s backup --pkg com.tencent.mm --out /sdcard/DataBackup --preserve --level 3\n"
        "  %s restore --in /sdcard/DataBackup/apps/com.tencent.mm/user_0 --install-apk --clean\n"
        "  %s restore --in /sdcard/DataBackup --pkg com.tencent.mm --clean\n",
        kToolName, kToolVersion, kToolName, kToolName, kToolName, kToolName, kToolName, kToolName, kToolName,
        kToolName);
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
                fprintf(stderr, "缺少 %s 的参数\n", name);
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
                fprintf(stderr, "未知压缩方式（zstd / gzip / none）\n");
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
        } else if (a == "--format") {
            // 只有 DataBackup 一种输出格式，--format 仅作旧脚本兼容占位，忽略具体取值
            std::string f = next("--format");
            if (f != "databackup" && f != "db") {
                log_warn("--format %s 已忽略：工具只产出 DataBackup 结构（apps/<包名>/user_<uid>/）", f.c_str());
            }
        } else if (a == "--kill") {
            std::string m = next("--kill");
            if (m == "none") bopt.kill = KillMode::None;
            else if (m == "am") bopt.kill = KillMode::Am;
            else if (m == "kill") bopt.kill = KillMode::Kill;
            else {
                fprintf(stderr, "未知 --kill 模式: %s\n", m.c_str());
                return 2;
            }
        } else if (a == "--in") {
            ropt.in_dir = next("--in");
            list_dir = ropt.in_dir;
        } else if (a == "--clean") {
            ropt.clean = true;
        } else if (a == "--install-apk") {
            ropt.install_apk = true;
        } else if (a == "--no-selinux") {
            ropt.do_selinux = false;
        } else if (a == "--no-verify") {
            ropt.verify_owner = false;
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
            fprintf(stderr, "未知参数: %s\n", a.c_str());
            return 2;
        }
    }

    if (cmd == "backup") return run_backup(bopt);
    if (cmd == "restore") return run_restore(ropt);
    if (cmd == "list") {
        if (list_dir.empty()) {
            fprintf(stderr, "list 需要 --in <备份目录>\n");
            return 2;
        }
        return run_list(list_dir, bopt.pkg);
    }
    if (cmd == "info") {
        if (bopt.pkg.empty()) {
            fprintf(stderr, "info 需要 --pkg <包名>\n");
            return 2;
        }
        return run_info(bopt.pkg, bopt.user);
    }

    fprintf(stderr, "未知子命令: %s\n", cmd.c_str());
    print_usage();
    return 2;
}
