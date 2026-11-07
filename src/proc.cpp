#include "proc.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common.h"

namespace ozj {

namespace {

ExecResult wait_with_pipe(pid_t pid, int pipe_fd) {
    ExecResult r;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipe_fd, buf, sizeof(buf))) > 0) {
        r.out.append(buf, static_cast<size_t>(n));
    }
    close(pipe_fd);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) {
        r.code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        r.code = 128 + WTERMSIG(status);
    } else {
        r.code = -1;
    }
    return r;
}

}  // namespace

ExecResult run_exec(const std::vector<std::string>& argv) {
    if (argv.empty()) return ExecResult{-1, ""};
    int fds[2];
    if (pipe(fds) != 0) {
        return ExecResult{-1, std::string("pipe: ") + strerror(errno)};
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return ExecResult{-1, std::string("fork: ") + strerror(errno)};
    }
    if (pid == 0) {
        // 子进程
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(fds[1]);
    return wait_with_pipe(pid, fds[0]);
}

ExecResult run_shell(const std::string& cmd) {
    log_debug("sh: %s", cmd.c_str());
    int fds[2];
    if (pipe(fds) != 0) {
        return ExecResult{-1, std::string("pipe: ") + strerror(errno)};
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return ExecResult{-1, std::string("fork: ") + strerror(errno)};
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }
    close(fds[1]);
    return wait_with_pipe(pid, fds[0]);
}

}  // namespace ozj
