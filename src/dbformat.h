#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "meta.h"
#include "sink.h"

namespace appbackup {

// DataBackup（Android-DataBackup-custom）的备份目录结构：
//   <saveDir>/apps/<pkg>/user_<uid>/{apk,user,user_de,data,obb,media}.tar.<suffix>
//   <saveDir>/apps/<pkg>/user_<uid>/<type>.tar.<suffix>.md5 / .manifest
//   <saveDir>/apps/<pkg>/user_<uid>/package_restore_config.json  (Gson 序列化的 PackageEntity)
// 主备份目录是 user_<uid>；被 preserve 归档的旧版本是 user_<uid>@<preserveId>。
// 本模块负责写/读这套结构，使两边备份可以互相恢复。

// 与上游 PackagePermission 对齐：op 是 AppOps 代码（-1 == OP_NONE），mode 是 AppOps 模式
// （-1 表示未知，JSON 里写 null —— 上游 PackagePermission.mode 本身就是可空）
struct DbFormatInfo {
    bool valid = false;
    std::string pkg;
    int user = 0;
    int uid = -1;
    std::string compression_type;   // TAR / ZSTD / LZ4
    long long preserve_id = 0;
    std::string label;
    std::string version_name;
    long long version_code = 0;
    long long first_install_time = 0;   // epoch ms
    long long last_update_time = 0;     // epoch ms
    long long app_flags = 0;            // ApplicationInfo.flags（上游 packageInfo.flags）
    // extraInfo.permissions / ssaid（上游备份会写、恢复会按 dataStates 决定是否应用）
    std::vector<PermEntry> permissions;
    std::string ssaid;
    bool permission_selected = false;   // dataStates.permissionState == Selected
    bool ssaid_selected = false;        // dataStates.ssaidState == Selected
};

// 目录名（user_0 / user_0@1699999999999）或 package_restore_config.json 命中即认为是 DataBackup 备份目录
bool is_databackup_dir(const std::string& dir);

// 生成备份版本目录：<saveDir>/apps/<pkg>/user_<user>@<preserveId>
// label 非空时应用目录用 <label>_<pkg>（与上游 DataBackup 命名一致），否则退回 <pkg>。
std::string databackup_version_dir(const std::string& save_dir, const std::string& pkg, int user,
                                   long long preserve_id, const std::string& label = std::string());

// 读 package_restore_config.json；文件缺失时退化为从目录名推断
bool read_databackup_info(const std::string& dir, DbFormatInfo* out);

// 探测某类型的归档实际路径（依次试 .tar.zst / .tar.gz / .tar / .tar.lz4），找不到返回空
std::string find_databackup_archive(const std::string& dir, const std::string& type);

// 写 package_restore_config.json（Gson 结构，DataBackup 可直接读取）
bool write_databackup_config(const std::string& dir, const DbFormatInfo& info,
                             const std::vector<DataEntry>& entries, const std::string& backup_dir);

// 给归档写 <path>.md5（DataBackup 恢复前会校验）
bool write_md5_sidecar(const std::string& archive_path);

// 从包管理器读 label / versionName / versionCode（尽力而为，失败留空）
void fill_package_display_info(const std::string& pkg, int user, DbFormatInfo* info);

// 采集 permissions（对齐上游 getPermissions：requestedPermissions + isGranted + AppOps op/mode）
void fill_package_permissions(const std::string& pkg, int user, int uid, DbFormatInfo* info);

// 读 /data/system/users/<user>/settings_ssaid.xml 里该 uid 的 SSAID；没有则空
std::string read_ssaid_value(int user, int uid);

// 写回 SSAID（按 settings_ssaid.xml 的 XML 结构原地改）；返回是否写到文件
bool write_ssaid_value(int user, int uid, const std::string& pkg, const std::string& value);

}  // namespace appbackup
