#pragma once

#include <anof/application/ports/InstancePreparation.h>

#include <utility>
#include <array>
#include <sys/stat.h>

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

struct InstanceRoot {
    std::filesystem::path root;
    OwnedFd rootFd;
};

struct InstanceDirectories {
    std::filesystem::path root;
    OwnedFd rootFd;
    OwnedFd anofFd;
};

ErrorCode filesystemError(int error) noexcept;
Result<InstanceRoot> openInstanceRoot(const std::filesystem::path& root);
Result<InstanceDirectories> openInstanceDirectories(const std::filesystem::path& root);
// name 必须是单个普通路径组件；不创建。检查类型后才以读写模式打开，拒绝硬链接。
Result<OwnedFd> openPrivateFile(int directory, const char* name, bool writable);
// 仅打开、验证固定内容目录；要求与 .anof 同设备，不创建或修复。
Result<std::array<OwnedFd, 3>> openContentDirectories(int anof);
// 与目录打开共用的纯属性检查，允许测试合成跨设备证据而不挂载文件系统。
Result<void> validateContentDirectory(const struct stat& held, const struct stat& named, dev_t device);
Result<void> validatePrivateFile(int fd, bool writable);
// 仅借用本适配器的 lease；借用期间必须保持 lease 存活。
Result<int> instanceDirectory(const InstanceLease& lease);
Result<int> instanceRootDirectory(const InstanceLease& lease);
Result<int> instanceLockFile(const InstanceLease& lease);
Result<void> validateInstanceLease(const InstanceLease& lease);
Result<std::unique_ptr<InstanceLease>> acquireInstanceLease(InstanceDirectories directories, bool createLock);

}  // namespace anof::infrastructure::system::detail
