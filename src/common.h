// appbackup —— 应用数据打包/恢复工具（C++17，root）
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace appbackup {

constexpr const char* kToolName = "appbackup";
constexpr const char* kToolVersion = "2.0.0";

// 全局日志级别：0=quiet 1=normal 2=verbose
extern int g_log_level;

void log_msg(const char* level, const char* fmt, va_list ap);
void log_info(const char* fmt, ...);
void log_warn(const char* fmt, ...);
void log_error(const char* fmt, ...);
void log_debug(const char* fmt, ...);

std::string strfmt(const char* fmt, ...);

// 去掉首尾空白与末尾换行
std::string trim(const std::string& s);

// 字符串切分（按分隔符，跳过空段）
std::vector<std::string> split(const std::string& s, char sep);

bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);

// 以 1024 进制的人类可读大小
std::string human_size(uint64_t bytes);

// 当前时间 ISO8601（本地时区）
std::string now_iso8601();

}  // namespace appbackup
