#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <sys/types.h>

namespace anof::test {
// 在原等待期限已经失败后读取，不增加被测进程的等待期限，也不读取业务文件。
inline void processSnapshot(pid_t pid) {
    const auto root = std::filesystem::path("/proc") / std::to_string(pid);
    std::set<std::string> sockets;
    std::error_code error;
    for (std::filesystem::directory_iterator it(root / "fd", error), end; !error && it != end; it.increment(error)) {
        const auto target = std::filesystem::read_symlink(it->path(), error).string();
        if (error) break;
        if (target.rfind("socket:[", 0) == 0) sockets.insert(target.substr(8, target.size() - 9));
        if (target == "anon_inode:[timerfd]") {
            std::ifstream info(root / "fdinfo" / it->path().filename());
            std::string line;
            while (std::getline(info, line))
                std::fprintf(stderr, "PROC_TRACE pid=%ld timerfd=%s %s\n", static_cast<long>(pid),
                             it->path().filename().c_str(), line.c_str());
        }
    }
    std::ifstream tcp(root / "net/tcp");
    std::string line;
    while (std::getline(tcp, line)) {
        std::istringstream fields(line);
        std::string field;
        for (int i = 0; i < 10 && fields >> field; ++i) {
            if (i == 9 && sockets.count(field))
                std::fprintf(stderr, "PROC_TRACE pid=%ld tcp=%s\n", static_cast<long>(pid), line.c_str());
        }
    }
    error.clear();
    for (std::filesystem::directory_iterator it(root / "task", error), end; !error && it != end; it.increment(error)) {
        std::ifstream channel(it->path() / "wchan");
        std::string wait;
        channel >> wait;
        std::fprintf(stderr, "PROC_TRACE pid=%ld tid=%s wchan=%s\n", static_cast<long>(pid),
                     it->path().filename().c_str(), wait.c_str());
    }
}
}  // namespace anof::test
