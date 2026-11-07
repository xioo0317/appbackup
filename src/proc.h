#pragma once

#include <string>
#include <vector>

namespace ozj {

struct ExecResult {
    int code = -1;
    std::string out;   // 合并后的 stdout+stderr
};

// fork+execvp 执行，捕获 stdout/stderr（不经过 shell，参数不转义）
ExecResult run_exec(const std::vector<std::string>& argv);

// 经过 /bin/sh -c 执行（用于管道场景，例如 zstd | tar）
ExecResult run_shell(const std::string& cmd);

}  // namespace ozj
