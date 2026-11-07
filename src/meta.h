#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace appbackup {

// 五类应用数据 + APK
extern const char* const kDataTypeUser;
extern const char* const kDataTypeUserDe;
extern const char* const kDataTypeData;
extern const char* const kDataTypeObb;
extern const char* const kDataTypeMedia;
extern const char* const kDataTypeApk;

std::vector<std::string> all_data_types();

// 类型 -> 目录模板（%d 为 user id）；apk 由 pm path 推导，不在模板表里
std::string data_type_dir_template(const std::string& type);

// 判断类型是否属于外部存储（/data/media，属主 chown 时 gid 要沿用目标目录现有 gid）
bool data_type_is_external(const std::string& type);

struct DataEntry {
    std::string type;
    std::string src_path;     // 打包时的绝对源目录
    std::string arc_name;     // 归档内顶层名（包名，apk 为 "apk"）
    std::string archive;      // 相对备份目录的归档文件名
    std::string context;      // 备份时该目录的 SELinux context（恢复时 chcon 用）
    long gid = 0;             // 备份时目录的 gid
    long uid = 0;
    bool exists = false;
    uint64_t files = 0;
    uint64_t bytes = 0;       // 归档（压缩后）大小 → displayStats
    uint64_t raw_bytes = 0;   // 原始 tar 大小（兜底）
    uint64_t src_bytes = 0;   // 源目录文件大小总和（含被排除项）→ dataStats
};

// 与上游 PackagePermission 对齐：op 是 AppOps 代码（-1 == OP_NONE），mode 是 AppOps 模式
// （-1 表示未知，JSON 里写 null —— 上游 PackagePermission.mode 本身就是可空）
struct PermEntry {
    std::string name;
    bool granted = false;
    int op = -1;
    int mode = -1;
};

struct BackupMeta {
    std::string tool;
    std::string tool_version;
    std::string pkg;
    int user = 0;
    int uid = -1;
    int gid = -1;
    std::string created_at;
    std::string device;
    std::string sdk;
    std::string compression;  // none | gzip
    std::vector<DataEntry> entries;
    // 上游 package_restore_config.json 的 extraInfo.permissions / ssaid
    std::vector<PermEntry> permissions;
    std::string ssaid;
    bool permission_selected = false;
    bool ssaid_selected = false;
};

std::string backup_meta_to_json(const BackupMeta& meta);
bool backup_meta_from_json(const std::string& text, BackupMeta* meta);

bool write_meta_file(const std::string& path, const BackupMeta& meta);
bool read_meta_file(const std::string& path, BackupMeta* meta);

// 简易 JSON 读取辅助（供 restore 读取外部手工编辑的 meta 时容错）
std::string json_get_string(const std::string& text, const std::string& key);
// 取数字字段（找不到返回 -1）
long long json_get_number(const std::string& text, const std::string& key);

}  // namespace appbackup
