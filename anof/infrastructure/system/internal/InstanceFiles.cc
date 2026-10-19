#include <anof/infrastructure/system/internal/InstanceFiles.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace anof::infrastructure::system::detail {

OwnedFd::~OwnedFd() {
    // Linux close 遇 EINTR 也不可重试，以免关闭已被复用的描述符。
    if (fd_ >= 0) (void)::close(fd_);
}
OwnedFd& OwnedFd::operator=(OwnedFd&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) (void)::close(fd_);
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

ErrorCode filesystemError(int error) noexcept {
    switch (error) {
    case ENOENT: return ErrorCode::NotFound;
    case ELOOP: case ENOTDIR: case EISDIR: return ErrorCode::IntegrityError;
    case ENOSPC: case EDQUOT: return ErrorCode::StorageFull;
    case ENAMETOOLONG: return ErrorCode::InvalidArgument;
    default: return ErrorCode::InternalError;
    }
}

namespace {
Result<void> directoryPermissions(int fd, bool privateDirectory) {
    struct stat status{};
    if (::fstat(fd, &status) != 0) return filesystemError(errno);
    if (!S_ISDIR(status.st_mode)) return ErrorCode::IntegrityError;
    const auto permissions = status.st_mode & 07777;
    if (privateDirectory) {
        if (status.st_uid != ::geteuid() || permissions != 0700) return ErrorCode::IntegrityError;
    } else {
        if ((status.st_uid != 0 && status.st_uid != ::geteuid()) || (permissions & 06000)) return ErrorCode::IntegrityError;
        if ((permissions & 0022) && !(status.st_uid == 0 && (permissions & S_ISVTX))) return ErrorCode::IntegrityError;
    }
    return {};
}
}

Result<InstanceRoot> openInstanceRoot(const std::filesystem::path& root) {
    if (!root.is_absolute() || root.native().find('\0') != std::string::npos) return ErrorCode::InvalidArgument;
    std::vector<std::string> components;
    for (const auto& part : root.relative_path()) {
        if (part == "..") return ErrorCode::InvalidArgument;
        if (!part.empty() && part != ".") components.push_back(part.native());
    }
    OwnedFd current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (current.get() < 0) return filesystemError(errno);
    auto valid = directoryPermissions(current.get(), components.empty());
    if (!valid) return valid.error();
    std::filesystem::path absolute = "/";
    for (std::size_t i = 0; i < components.size(); ++i) {
        OwnedFd next(::openat(current.get(), components[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (next.get() < 0) return filesystemError(errno);
        valid = directoryPermissions(next.get(), i + 1 == components.size());
        if (!valid) return valid.error();
        absolute /= components[i];
        current = std::move(next);
    }
    return InstanceRoot{ std::move(absolute), std::move(current) };
}

Result<InstanceDirectories> openInstanceDirectories(const std::filesystem::path& root) {
    auto opened = openInstanceRoot(root);
    if (!opened) return opened.error();
    auto& current = opened.value().rootFd;
    OwnedFd anof(::openat(current.get(), ".anof", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (anof.get() < 0) return filesystemError(errno);
    auto valid = directoryPermissions(anof.get(), true);
    if (!valid) return valid.error();
    return InstanceDirectories{ std::move(opened.value().root), std::move(current), std::move(anof) };
}

Result<void> validateContentDirectory(const struct stat& held, const struct stat& named, dev_t device) {
    if (!S_ISDIR(held.st_mode) || held.st_uid != ::geteuid() || (held.st_mode & 07777) != 0700 ||
        held.st_dev != device || held.st_dev != named.st_dev || held.st_ino != named.st_ino) return ErrorCode::IntegrityError;
    return {};
}

Result<std::array<OwnedFd, 3>> openContentDirectories(int anof) {
    struct stat base{};
    if (::fstat(anof, &base) != 0) return filesystemError(errno);
    std::array<OwnedFd, 3> result;
    const char* names[] = { "uploads", "objects", "quarantine" };
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = OwnedFd(::openat(anof, names[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (result[i].get() < 0) return filesystemError(errno);
        auto valid = directoryPermissions(result[i].get(), true);
        if (!valid) return valid.error();
        struct stat held{}, named{};
        if (::fstat(result[i].get(), &held) || ::fstatat(anof, names[i], &named, AT_SYMLINK_NOFOLLOW)) return filesystemError(errno);
        valid = validateContentDirectory(held, named, base.st_dev);
        if (!valid) return valid.error();
    }
    return result;
}

Result<void> validatePrivateFile(int fd, bool writable) {
    struct stat status{};
    if (::fstat(fd, &status) != 0) return filesystemError(errno);
    const auto permissions = status.st_mode & 07777;
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink != 1 ||
        (permissions != 0600 && (writable || permissions != 0400))) return ErrorCode::IntegrityError;
    return {};
}

Result<OwnedFd> openPrivateFile(int directory, const char* name, bool writable) {
    if (!name || !*name || std::strchr(name, '/') || std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
        return ErrorCode::InvalidArgument;
    }
    // O_PATH 先检查，不因设备 open 的副作用或 FIFO 等待而触碰未认可的对象。
    OwnedFd inspected(::openat(directory, name, O_PATH | O_NOFOLLOW | O_CLOEXEC));
    if (inspected.get() < 0) return filesystemError(errno);
    auto valid = validatePrivateFile(inspected.get(), writable);
    if (!valid) return valid.error();
    OwnedFd file(::openat(directory, name, (writable ? O_RDWR : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (file.get() < 0) return filesystemError(errno);
    valid = validatePrivateFile(file.get(), writable);
    if (!valid) return valid.error();
    struct stat before{}, after{};
    if (::fstat(inspected.get(), &before) != 0 || ::fstat(file.get(), &after) != 0) return filesystemError(errno);
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino) return ErrorCode::IntegrityError;
    return file;
}

}  // namespace anof::infrastructure::system::detail
