#pragma once

#include <string>
#include <vector>

namespace appbackup {

struct RestoreOptions {
    std::string pkg;                  // 为空时从 meta.json / package_restore_config.json 读
    int user = 0;
    std::string in_dir;               // 备份目录（版本目录 / 应用目录 / 备份根目录，自动下探）
    std::vector<std::string> types;   // 空表示全部
    bool clean = false;               // 干净恢复：先清空目标第一层（保留 lib/cache/code_cache/no_backup/.ota）
    bool do_selinux = true;
    // 与上游 AbstractRestoreService 一致：PACKAGE_APK 最先恢复（安装），数据类才拿得到 uid。
    // 应用已被卸载时，默认从备份里的 apk 归档自动安装，装完再恢复数据。
    bool install_apk = true;          // 应用未安装时自动装 APK；--no-apk 关闭
    bool force_reinstall = false;     // 即使已安装也覆盖安装（--install-apk）
    bool verify_owner = true;
    bool force_stop = true;
    bool verify_checksum = true;      // 恢复前校验 <归档>.md5（与 DataBackup 一致，缺失则跳过）
    bool force_checksum = false;      // 校验不一致时仍强制恢复
};

// 返回进程退出码：0 成功
int run_restore(const RestoreOptions& opt);

// 列出备份内容（list 子命令）
int run_list(const std::string& in_dir, const std::string& pkg_hint = std::string());

// 打印设备侧信息（info 子命令）
int run_info(const std::string& pkg, int user);

}  // namespace appbackup
