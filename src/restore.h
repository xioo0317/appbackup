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
    bool install_apk = false;         // 先装 APK（多 APK 走 install-create 会话），再恢复数据
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
