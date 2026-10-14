#include <anof/infrastructure/system/LinuxInstanceLock.h>
#include <anof/infrastructure/system/internal/InstanceFiles.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <cerrno>
#include <new>

namespace anof::infrastructure::system {
namespace {

class LinuxInstanceLease final : public InstanceLease {
public:
    LinuxInstanceLease(detail::InstanceDirectories directories, detail::OwnedFd lock)
        : directories_(std::move(directories)), lock_(std::move(lock)) {}
    const std::filesystem::path& root() const noexcept override { return directories_.root; }
    int directory() const noexcept { return directories_.anofFd.get(); }
    int rootDirectory() const noexcept { return directories_.rootFd.get(); }
    int lockFile() const noexcept { return lock_.get(); }
private:
    detail::InstanceDirectories directories_;
    detail::OwnedFd lock_;
};

Result<detail::OwnedFd> openLock(int directory, bool createLock) {
    auto existing = detail::openPrivateFile(directory, "instance.lock", true);
    if (existing || existing.error().code != ErrorCode::NotFound || !createLock) return existing;
    detail::OwnedFd created(::openat(directory, "instance.lock", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600));
    if (created.get() < 0) {
        // 创建竞争后仅重开一次；不循环争抢、不删除获胜者创建的文件。
        if (errno == EEXIST) return detail::openPrivateFile(directory, "instance.lock", true);
        return detail::filesystemError(errno);
    }
    auto valid = detail::validatePrivateFile(created.get(), true);
    if (!valid) return valid.error();
    return created;
}

class LinuxInstanceLockProvider final : public InstanceLockProvider {
public:
    Result<std::unique_ptr<InstanceLease>> acquire(const std::filesystem::path& root) override {
        try {
            auto directories = detail::openInstanceDirectories(root);
            if (!directories) return directories.error();
            return detail::acquireInstanceLease(std::move(directories).value(), true);
        } catch (const std::bad_alloc&) {
            return ErrorCode::InternalError;
        } catch (const std::filesystem::filesystem_error&) {
            return ErrorCode::InternalError;
        }
    }
};
}

Result<std::unique_ptr<InstanceLease>> detail::acquireInstanceLease(InstanceDirectories directories, bool createLock) {
    try {
        auto lock = openLock(directories.anofFd.get(), createLock);
        if (!lock) return lock.error();
        if (::flock(lock.value().get(), LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) return ErrorCode::Conflict;
            return detail::filesystemError(errno);
        }
        // 检查持锁对象仍对应固定名字；不声称防御同 UID 恶意持续替换。
        struct stat held{}, named{};
        if (::fstat(lock.value().get(), &held) != 0 ||
            ::fstatat(directories.anofFd.get(), "instance.lock", &named, AT_SYMLINK_NOFOLLOW) != 0) {
            return detail::filesystemError(errno);
        }
        if (held.st_dev != named.st_dev || held.st_ino != named.st_ino) return ErrorCode::IntegrityError;
        return std::unique_ptr<InstanceLease>(new LinuxInstanceLease(std::move(directories), std::move(lock).value()));
    } catch (const std::bad_alloc&) { return ErrorCode::InternalError; }
}

Result<int> detail::instanceDirectory(const InstanceLease& lease) {
    const auto* concrete = dynamic_cast<const LinuxInstanceLease*>(&lease);
    if (!concrete) return ErrorCode::InvalidArgument;
    return concrete->directory();
}

Result<int> detail::instanceRootDirectory(const InstanceLease& lease) {
    const auto* concrete = dynamic_cast<const LinuxInstanceLease*>(&lease);
    if (!concrete) return ErrorCode::InvalidArgument;
    return concrete->rootDirectory();
}
Result<int> detail::instanceLockFile(const InstanceLease& lease) {
    const auto* concrete = dynamic_cast<const LinuxInstanceLease*>(&lease);
    if (!concrete) return ErrorCode::InvalidArgument;
    return concrete->lockFile();
}
Result<void> detail::validateInstanceLease(const InstanceLease& lease) {
    const auto* concrete = dynamic_cast<const LinuxInstanceLease*>(&lease);
    if (!concrete) return ErrorCode::InvalidArgument;
    auto reopened = openInstanceDirectories(lease.root());
    if (!reopened) return reopened.error();
    for (const auto& pair : { std::pair<int, int>{ concrete->rootDirectory(), reopened.value().rootFd.get() },
                             { concrete->directory(), reopened.value().anofFd.get() } }) {
        struct stat held{}, named{};
        if (::fstat(pair.first, &held) != 0 || ::fstat(pair.second, &named) != 0) return filesystemError(errno);
        if (held.st_dev != named.st_dev || held.st_ino != named.st_ino) return ErrorCode::IntegrityError;
    }
    auto valid = validatePrivateFile(concrete->lockFile(), true);
    if (!valid) return valid.error();
    struct stat held{}, named{};
    if (::fstat(concrete->lockFile(), &held) != 0 || ::fstatat(concrete->directory(), "instance.lock", &named, AT_SYMLINK_NOFOLLOW) != 0) return filesystemError(errno);
    if (held.st_dev != named.st_dev || held.st_ino != named.st_ino) return ErrorCode::IntegrityError;
    return {};
}

Result<std::unique_ptr<InstanceLockProvider>> makeLinuxInstanceLockProvider() {
    try {
        return std::unique_ptr<InstanceLockProvider>(new LinuxInstanceLockProvider);
    } catch (const std::bad_alloc&) {
        return ErrorCode::InternalError;
    }
}

}  // namespace anof::infrastructure::system
