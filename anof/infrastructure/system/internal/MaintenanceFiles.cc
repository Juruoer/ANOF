#include <anof/infrastructure/system/internal/MaintenanceFiles.h>
#include <fcntl.h>
#include <sodium.h>
#include <unistd.h>
namespace anof::infrastructure::system::maintenance {
MaintenanceFailure failure(const MaintenanceContext& c, MaintenanceStage stage, MaintenanceError code, MaintenanceOutcome outcome) { return { code, stage, c.operationId, outcome, MaintenanceReportStatus::Incomplete, {}, {} }; }
MaintenanceFailure ioFailure(const MaintenanceContext& c, MaintenanceStage stage, bool effect) { return failure(c, stage, effect ? MaintenanceError::PublicationOutcomeUnknown : (errno == ENOSPC || errno == EDQUOT ? MaintenanceError::StorageFull : MaintenanceError::IoError), effect ? MaintenanceOutcome::Unknown : MaintenanceOutcome::NotStarted); }
Result<OwnedFd> directory(int parent, const std::string& name, bool create) {
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos || name.find('\0') != std::string::npos) return ErrorCode::InvalidArgument;
    if (create && ::mkdirat(parent, name.c_str(), 0700) && errno != EEXIST) return detail::filesystemError(errno);
    OwnedFd fd(::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (fd.get() < 0) return detail::filesystemError(errno);
    struct stat p{}, held{}, named{};
    if (::fstat(parent, &p) || ::fstat(fd.get(), &held) || ::fstatat(parent, name.c_str(), &named, AT_SYMLINK_NOFOLLOW)) return detail::filesystemError(errno);
    auto valid = detail::validateContentDirectory(held, named, p.st_dev);
    if (!valid) return valid.error();
    return fd;
}
Result<FileEvidence> evidence(int fd) {
    struct stat s{};
    if (::fstat(fd, &s)) return detail::filesystemError(errno);
    auto size = ByteCount::from(s.st_size);
    if (!size) return size.error();
    auto ns = [](const timespec& t) -> Result<std::int64_t> {if(t.tv_sec>INT64_MAX/1000000000||t.tv_sec<INT64_MIN/1000000000)return ErrorCode::IntegrityError;auto base=static_cast<std::int64_t>(t.tv_sec)*1000000000;if(t.tv_nsec<0||t.tv_nsec>=1000000000||base>INT64_MAX-t.tv_nsec)return ErrorCode::IntegrityError;return base+t.tv_nsec; };
    auto mt = ns(s.st_mtim), ct = ns(s.st_ctim);
    if (!mt || !ct) return ErrorCode::IntegrityError;
    return FileEvidence{ static_cast<std::uint64_t>(s.st_dev), static_cast<std::uint64_t>(s.st_ino), size.value(), mt.value(), ct.value() };
}
bool same(const FileEvidence& a, const FileEvidence& b) { return a.device == b.device && a.inode == b.inode && a.size == b.size && a.mtimeNs == b.mtimeNs && a.ctimeNs == b.ctimeNs; }
Result<void> writeAll(int fd, const void* data, std::size_t size, Io* io) {
    auto* bytes = static_cast<const unsigned char*>(data);
    while (size) {
        auto n = (io ? io->write(fd, bytes, size) : ::write(fd, bytes, size));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return detail::filesystemError(errno);
        bytes += n;
        size -= static_cast<std::size_t>(n);
    }
    return {};
}
Result<BackupFileDigest> hashCopy(const MaintenanceContext& c, int source, int destination, Io* io) {
    auto before = evidence(source);
    if (!before) return before.error();
    if (::lseek(source, 0, SEEK_SET) < 0) return detail::filesystemError(errno);
    crypto_hash_sha256_state hash;
    crypto_hash_sha256_init(&hash);
    std::array<unsigned char, kMaintenanceBlockBytes> block{};
    ByteCount size;
    for (;;) {
        if (c.cancelled.load()) return ErrorCode::InvalidArgument;
        auto n = (io ? io->read(source, block.data(), block.size()) : ::read(source, block.data(), block.size()));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return detail::filesystemError(errno);
        if (!n) break;
        auto sum = size.add(n);
        if (!sum) return sum.error();
        size = sum.value();
        if (size.value() > before.value().size.value()) return ErrorCode::IntegrityError;
        crypto_hash_sha256_update(&hash, block.data(), static_cast<unsigned long long>(n));
        if (destination >= 0) {
            auto w = writeAll(destination, block.data(), static_cast<std::size_t>(n), io);
            if (!w) return w.error();
        }
    }
    auto after = evidence(source);
    if (!after || !same(before.value(), after.value()) || size != before.value().size) return ErrorCode::IntegrityError;
    std::array<std::uint8_t, 32> digest{};
    crypto_hash_sha256_final(&hash, digest.data());
    return BackupFileDigest{ size, Sha256(digest) };
}
Result<Location> locate(const InstanceLease& lease, const MaintenanceFileKey& key, bool create) {
    auto valid = detail::validateInstanceLease(lease);
    if (!valid) return valid.error();
    auto base = detail::instanceDirectory(lease);
    if (!base) return base.error();
    const auto path = std::filesystem::path(maintenanceStorageKey(key));
    OwnedFd current(::dup(base.value()));
    auto component = path.begin();
    while (component != path.end()) {
        auto name = component->string();
        ++component;
        if (component == path.end()) return Location{ std::move(current), name };
        auto next = directory(current.get(), name, create);
        if (!next) return next.error();
        current = std::move(next).value();
    }
    return ErrorCode::InvalidArgument;
}
Result<void> Destination::validate() const {
    auto root = detail::openInstanceRoot(stagePath);
    if (!root) return root.error();
    struct stat a{}, b{};
    if (::fstat(stage.get(), &a) || ::fstat(root.value().rootFd.get(), &b)) return detail::filesystemError(errno);
    if (a.st_dev != b.st_dev || a.st_ino != b.st_ino) return ErrorCode::IntegrityError;
    auto currentParent = detail::openControlledDirectory(finalPath.parent_path());
    if (!currentParent) return currentParent.error();
    if (::fstat(parent.get(), &a) || ::fstat(currentParent.value().rootFd.get(), &b) || a.st_dev != b.st_dev || a.st_ino != b.st_ino) return ErrorCode::IntegrityError;
    std::array<std::uint8_t, 32> hash{};
    auto basename = finalPath.filename().string();
    crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(basename.data()), basename.size());
    auto lockName = ".anof-destination-" + Sha256(hash).toString() + ".lock";
    if (!detail::validatePrivateFile(lock.get(), true) || ::fstat(lock.get(), &a) || ::fstatat(parent.get(), lockName.c_str(), &b, AT_SYMLINK_NOFOLLOW) || a.st_dev != b.st_dev || a.st_ino != b.st_ino) return ErrorCode::IntegrityError;
    return {};
}
Result<const Destination*> destination(const MaintenanceDestination& d) {
    auto* p = dynamic_cast<const Destination*>(&d);
    if (!p) return ErrorCode::InvalidArgument;
    auto valid = p->validate();
    if (!valid) return valid.error();
    return p;
}
}  // namespace anof::infrastructure::system::maintenance
