#include "common.h"

#include <ctime>
#include <cctype>

namespace ozj {

int g_log_level = 1;

void log_msg(const char* level, const char* fmt, va_list ap) {
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    fprintf(stderr, "[%s] %s\n", level, buf);
    fflush(stderr);
}

void log_info(const char* fmt, ...) {
    if (g_log_level < 1) return;
    va_list ap;
    va_start(ap, fmt);
    log_msg("info", fmt, ap);
    va_end(ap);
}

void log_warn(const char* fmt, ...) {
    if (g_log_level < 1) return;
    va_list ap;
    va_start(ap, fmt);
    log_msg("warn", fmt, ap);
    va_end(ap);
}

void log_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_msg("error", fmt, ap);
    va_end(ap);
}

void log_debug(const char* fmt, ...) {
    if (g_log_level < 2) return;
    va_list ap;
    va_start(ap, fmt);
    log_msg("debug", fmt, ap);
    va_end(ap);
}

std::string strfmt(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string human_size(uint64_t bytes) {
    static const char* units[] = {"B", "K", "M", "G", "T"};
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        i++;
    }
    if (i == 0) return strfmt("%llu%s", static_cast<unsigned long long>(bytes), units[i]);
    return strfmt("%.1f%s", v, units[i]);
}

std::string now_iso8601() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tmv);
    return std::string(buf);
}

}  // namespace ozj
