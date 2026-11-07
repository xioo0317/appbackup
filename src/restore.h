#pragma once

#include <string>
#include <vector>

namespace ozj {

struct RestoreOptions {
    std::string pkg;                  // 为空时从 meta.json 读
    int user = 0;
    std::string in_dir;               // 备份目录（含 meta.json）
    std::vector<std::string> types;   // 空表示全部
    bool clean = false;               // 干净恢复：先清空目标第一层（保留 lib/cache/code_cache/no_backup/.ota）
    bool do_selinux = true;
    bool install_apk = false;         // 先 pm install 备份里的 APK，再恢复数据
    bool verify_owner = true;
    bool force_stop = true;
};

// 返回进程退出码：0 成功
int run_restore(const RestoreOptions& opt);

// 列出备份内容（list 子命令）
int run_list(const std::string& in_dir);

// 打印设备侧信息（info 子命令）
int run_info(const std::string& pkg, int user);

}  // namespace ozj
