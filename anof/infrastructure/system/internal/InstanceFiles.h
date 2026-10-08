#pragma once

#include <anof/application/ports/InstancePreparation.h>

#include <utility>

namespace anof::infrastructure::system::detail {

class OwnedFd {
public:
    explicit OwnedFd(int fd = -1) noexcept : fd_(fd) {}
    ~OwnedFd();
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    OwnedFd& operator=(OwnedFd&& other) noexcept;
    int get() const noexcept { return fd_; }
private:
    int fd_;
};

struct InstanceDirectories {
    std::filesystem::path root;
    OwnedFd rootFd;
    OwnedFd anofFd;
};

ErrorCode filesystemError(int error) noexcept;
Result<InstanceDirectories> openInstanceDirectories(const std::filesystem::path& root);
// name 必须是单个普通路径组件；不创建。检查类型后才以读写模式打开，拒绝硬链接。
Result<OwnedFd> openPrivateFile(int directory, const char* name, bool writable);
Result<void> validatePrivateFile(int fd, bool writable);
// 仅借用本适配器的 lease；借用期间必须保持 lease 存活。
Result<int> instanceDirectory(const InstanceLease& lease);

}  // namespace anof::infrastructure::system::detail
