#pragma once

// 恢复策略：与 DataBackup（Android-DataBackup-custom）PackagesRestoreUtil 逐条对齐的
// 目录映射与排除规则。抽成头文件是为了让主机侧测试直接验证「发布出去的就是测过的这份」，
// 避免测试里再抄一遍规则导致两边漂移。

#include <cstdio>
#include <string>
#include <vector>

#include "fsops.h"
#include "meta.h"
#include "tar_reader.h"

namespace appbackup {
namespace policy {

// 干净恢复时保留的目录名（与 DataBackup SELinux.cleanRestore 一致）
inline std::vector<std::string> keep_names() { return {"lib", "cache", "code_cache", "no_backup", ".ota"}; }

// 五类数据 → 目标目录；apk 返回空（由 pm 安装流程处理）
inline std::string target_dir(const std::string& type, int user, const std::string& pkg) {
    if (type == kDataTypeApk) return std::string();
    std::string tmpl = data_type_dir_template(type);
    if (tmpl.empty()) return std::string();
    char buf[256];
    snprintf(buf, sizeof(buf), tmpl.c_str(), user);
    return join_path(buf, pkg);
}

// 归档必须解到目标目录的**父目录**：归档内是 <pkg>/...，
// 与 DataBackup Tar.decompress(dst = dstDir) 一致。解到 dest 本身会得到 <pkg>/<pkg>/...。
inline std::string extract_root_for(const std::string& dest) { return dirname_of(dest); }

// 恢复排除项（DataBackup PackagesRestoreUtil.restoreData）：
//   - 五类数据统一排除 <pkg>/.ota|/cache|/lib|/code_cache|/no_backup
//     （lib 是备份机 APK 的 native 库，覆盖到新装的包上会因 ABI/版本不匹配闪退）
//   - data/obb/media 额外排除 Backup_* 与媒体库回收站/写入中文件（大小写两种写法）
inline void apply_restore_exclusions(const std::string& type, const std::string& pkg, ExtractOptions* opt) {
    for (const char* sub : {".ota", "cache", "lib", "code_cache", "no_backup"}) {
        opt->exclude_prefixes.push_back(pkg + "/" + sub);
    }
    if (type == kDataTypeData || type == kDataTypeObb || type == kDataTypeMedia) {
        for (const char* g : {"Backup_*", ".trashed-*", ".pending-*", ".Trashed-*", ".Pending-*"}) {
            opt->exclude_globs.push_back(g);
        }
    }
}

}  // namespace policy
}  // namespace appbackup
