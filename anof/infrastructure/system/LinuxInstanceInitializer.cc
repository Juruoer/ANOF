#include <anof/infrastructure/system/LinuxInstanceInitializer.h>
#include <anof/infrastructure/system/internal/InitializationFiles.h>
#include <anof/infrastructure/system/internal/InstanceFiles.h>
#include <anof/infrastructure/system/internal/BoundedFileRead.h>
#include <anof/infrastructure/config/internal/ConfigurationText.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <new>
#include <optional>

namespace anof::infrastructure::system {
namespace {
using detail::initializationError;
using detail::initializationErrno;
using detail::OwnedFd;
using detail::InitializationFileOps;

int renameNoReplace(int from, const char* source, int to, const char* target) {
    return static_cast<int>(::syscall(SYS_renameat2, from, source, to, target, RENAME_NOREPLACE));
}
struct ReadFile {
    std::string text;
    struct stat status;
};
InitResult<ReadFile> readFile(int directory, const char* name, std::size_t limit, InitStage stage) {
    auto file = detail::openPrivateFile(directory, name, false);
    if (!file) return initializationError(file.error().code, stage);
    auto text = detail::readBoundedFile(file.value().get(), limit, ErrorCode::IntegrityError);
    if (!text) return initializationError(text.error().code, stage);
    struct stat status{}, named{};
    if (::fstat(file.value().get(), &status) != 0 || ::fstatat(directory, name, &named, AT_SYMLINK_NOFOLLOW) != 0) return initializationErrno(errno, stage);
    if (status.st_dev != named.st_dev || status.st_ino != named.st_ino) return InitFailure{ InitError::IntegrityError, stage };
    return ReadFile{ std::move(text).value(), status };
}
InitResult<InstanceConfiguration> configuration(int directory, InitStage stage) {
    auto file = readFile(directory, "config.toml", 65536, stage);
    if (!file) return file.error();
    auto parsed = config::detail::parseConfiguration(file.value().text, true);
    if (!parsed) return initializationError(parsed.error().code, stage);
    return std::move(parsed).value();
}
InitResult<void> syncFile(int fd, const InitializationFileOps& ops, InitStage stage, bool published) {
    int result;
    do { result = ops.sync(fd); } while (result != 0 && errno == EINTR);
    if (result == 0) return {};
    if (published) return InitFailure{ InitError::PublicationOutcomeUnknown, stage };
    return initializationErrno(errno, stage);
}
InitResult<void> publish(int directory, int root, const char* name, std::string_view text, const InitializationIdentity& identity,
                         const InitializationFileOps& ops, InitStage stage, bool reuseTemporary) {
    const auto temporary = std::string(".") + name + "." + identity.operationId.toString() + ".tmp";
    OwnedFd file(::openat(directory, temporary.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600));
    bool reused = false;
    if (file.get() < 0) {
        if (errno != EEXIST || !reuseTemporary) return initializationErrno(errno, stage);
        // 只复用本 operation 的已完整写出文件；不截断、不改写、不扫描清理。
        auto previous = readFile(directory, temporary.c_str(), 4096, stage);
        if (!previous) return previous.error();
        auto marker = detail::parseFormat(previous.value().text, stage);
        if (!marker) return marker.error();
        if (marker.value() != identity.instanceId) return InitFailure{ InitError::IntegrityError, stage };
        auto opened = detail::openPrivateFile(directory, temporary.c_str(), true);
        if (!opened) return initializationError(opened.error().code, stage);
        file = std::move(opened).value();
        struct stat status{};
        if (::fstat(file.get(), &status) != 0) return initializationErrno(errno, stage);
        if (status.st_dev != previous.value().status.st_dev || status.st_ino != previous.value().status.st_ino) return InitFailure{ InitError::IntegrityError, stage };
        reused = true;
    }
    auto valid = detail::validatePrivateFile(file.get(), true);
    if (!valid) return initializationError(valid.error().code, stage);
    if (!reused) {
        std::size_t offset = 0;
        while (offset < text.size()) {
            const auto count = ops.write(file.get(), text.data() + offset, text.size() - offset);
            if (count < 0) { if (errno == EINTR) continue; return initializationErrno(errno, stage); }
            if (count == 0 || static_cast<std::size_t>(count) > text.size() - offset) return InitFailure{ InitError::IoError, stage };
            offset += static_cast<std::size_t>(count);
        }
    }
    auto synced = syncFile(file.get(), ops, stage, false);
    if (!synced) return synced.error();
    struct stat held{}, named{};
    if (::fstat(file.get(), &held) != 0 || ::fstatat(directory, temporary.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0) return initializationErrno(errno, stage);
    if (held.st_dev != named.st_dev || held.st_ino != named.st_ino) return InitFailure{ InitError::IntegrityError, stage };
    if (ops.renameNoReplace(directory, temporary.c_str(), directory, name) != 0) {
        // EIO 等错误不能证明名字未发布；已知未执行的冲突／权限等错误保留具体分类。
        if (errno == EIO || errno == EINTR) return InitFailure{ InitError::PublicationOutcomeUnknown, stage };
        return initializationErrno(errno, stage);
    }
    synced = syncFile(directory, ops, stage, true);
    if (!synced) return synced.error();
    return syncFile(root, ops, stage, true);
}
InitResult<void> emptyRoot(int directory) {
    struct stat status{};
    if (::fstatat(directory, ".anof", &status, AT_SYMLINK_NOFOLLOW) == 0) return InitFailure{ InitError::AlreadyExists, InitStage::CreateDirectory };
    if (errno != ENOENT) return initializationErrno(errno, InitStage::CreateDirectory);
    OwnedFd copy(::openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (copy.get() < 0) return initializationErrno(errno, InitStage::CreateDirectory);
    const int duplicate = ::fcntl(copy.get(), F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) return initializationErrno(errno, InitStage::CreateDirectory);
    DIR* opened = ::fdopendir(duplicate);
    if (!opened) { const int error = errno; ::close(duplicate); return initializationErrno(error, InitStage::CreateDirectory); }
    std::unique_ptr<DIR, int (*)(DIR*)> entries(opened, ::closedir);
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(entries.get());
        if (!entry) {
            if (errno) return initializationErrno(errno, InitStage::CreateDirectory);
            return {};
        }
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        return InitFailure{ std::strcmp(entry->d_name, ".anof") == 0 ? InitError::AlreadyExists : InitError::InvalidInput, InitStage::CreateDirectory };
    }
}

class LinuxInitializationLease final : public InitializationLease {
public:
    LinuxInitializationLease(std::unique_ptr<InstanceLease> lease, InitializationIdentity identity, struct stat pending,
                             InitializationFileOps ops, std::optional<InstanceConfiguration> config, bool resumed)
        : lease_(std::move(lease)), identity_(identity), pending_(pending), ops_(ops), configuration_(std::move(config)), resumed_(resumed) {}
    const InstanceLease& lease() const override { return *lease_; }
    const InitializationIdentity& identity() const override { return identity_; }
    InitResult<void> writeConfiguration(const InstanceConfiguration& value) override {
        constexpr auto stage = InitStage::WriteConfiguration;
        try {
            if (auto result = preflight(stage); !result) return result.error();
            if (resumed_ || configuration_) return InitFailure{ InitError::AlreadyExists, stage };
            auto text = config::detail::serializeConfiguration(value);
            if (!text) return initializationError(text.error().code, stage);
            struct stat existing{};
            if (::fstatat(directory(), "config.toml", &existing, AT_SYMLINK_NOFOLLOW) == 0) return InitFailure{ InitError::AlreadyExists, stage };
            if (errno != ENOENT) return initializationErrno(errno, stage);
            auto written = publish(directory(), root(), "config.toml", text.value(), identity_, ops_, stage, false);
            if (!written) return written.error();
            configuration_ = value;
            return {};
        } catch (const std::bad_alloc&) { return InitFailure{ InitError::InternalError, stage }; }
        catch (const std::filesystem::filesystem_error&) { return InitFailure{ InitError::IoError, stage }; }
    }
    InitResult<void> publishFormat() override {
        constexpr auto stage = InitStage::PublishFormat;
        published_ = false;
        try {
            if (auto result = preflight(stage); !result) return result.error();
            const auto configured = checkConfiguration(stage);
            if (configured) return *configured;
            auto marker = readFile(directory(), "instance.json", 4096, stage);
            if (marker) {
                if (auto result = matchingFormat(marker.value().text, stage); !result) return result.error();
                // 已发布内容不重写；重新同步保证上次发布结果未知后的显式 finish 可完成。
                auto file = detail::openPrivateFile(directory(), "instance.json", false);
                if (!file) return initializationError(file.error().code, stage);
                if (auto result = syncFile(file.value().get(), ops_, stage, true); !result) return result.error();
                if (auto result = syncFile(directory(), ops_, stage, true); !result) return result.error();
                if (auto result = syncFile(root(), ops_, stage, true); !result) return result.error();
                published_ = true;
                return {};
            }
            if (marker.error().code != InitError::NotFound) return marker.error();
            if (auto result = publish(directory(), root(), "instance.json", detail::formatText(identity_), identity_, ops_, stage, true); !result) return result.error();
            published_ = true;
            return {};
        } catch (const std::bad_alloc&) { return InitFailure{ InitError::InternalError, stage }; }
        catch (const std::filesystem::filesystem_error&) { return InitFailure{ InitError::IoError, stage }; }
    }
    InitResult<void> clearPending() override {
        constexpr auto stage = InitStage::ClearPending;
        if (!published_) return InitFailure{ InitError::IncompleteInitialization, stage };
        try {
            if (auto result = preflight(stage); !result) return result.error();
            const auto configured = checkConfiguration(stage);
            if (configured) return *configured;
            auto marker = readFile(directory(), "instance.json", 4096, stage);
            if (!marker) return marker.error();
            if (auto result = matchingFormat(marker.value().text, stage); !result) return result.error();
            // preflight 核对本次 pending 的内容及 inode；不删除未知操作标记。
            if (ops_.unlink(directory(), "initialization.pending", 0) != 0) {
                if (errno == EIO || errno == EINTR) return InitFailure{ InitError::PublicationOutcomeUnknown, stage };
                return initializationErrno(errno, stage);
            }
            return syncFile(directory(), ops_, stage, true);
        } catch (const std::bad_alloc&) { return InitFailure{ InitError::InternalError, stage }; }
        catch (const std::filesystem::filesystem_error&) { return InitFailure{ InitError::IoError, stage }; }
    }
private:
    int directory() const { return detail::instanceDirectory(*lease_).value(); }
    int root() const { return detail::instanceRootDirectory(*lease_).value(); }
    InitResult<void> preflight(InitStage stage) {
        auto valid = detail::validateInstanceLease(*lease_);
        if (!valid) return initializationError(valid.error().code, stage);
        auto pending = readFile(directory(), "initialization.pending", 4096, stage);
        if (!pending) return pending.error();
        auto identity = detail::parsePending(pending.value().text, stage);
        if (!identity) return identity.error();
        if (identity.value().instanceId != identity_.instanceId || identity.value().operationId != identity_.operationId ||
            pending.value().status.st_dev != pending_.st_dev || pending.value().status.st_ino != pending_.st_ino) return InitFailure{ InitError::IntegrityError, stage };
        return {};
    }
    std::optional<InitFailure> checkConfiguration(InitStage stage) {
        if (!configuration_) return InitFailure{ InitError::IncompleteInitialization, stage };
        auto stored = configuration(directory(), stage);
        if (!stored) return stored.error();
        if (!config::detail::sameConfiguration(*configuration_, stored.value())) return InitFailure{ InitError::IntegrityError, stage };
        return {};
    }
    InitResult<void> matchingFormat(std::string_view text, InitStage stage) {
        auto marker = detail::parseFormat(text, stage);
        if (!marker) return marker.error();
        if (marker.value() != identity_.instanceId) return InitFailure{ InitError::IntegrityError, stage };
        return {};
    }
    std::unique_ptr<InstanceLease> lease_;
    InitializationIdentity identity_;
    struct stat pending_;
    InitializationFileOps ops_;
    std::optional<InstanceConfiguration> configuration_;
    bool resumed_;
    bool published_ = false;
};

class LinuxInstanceInitializer final : public InstanceInitializer {
public:
    explicit LinuxInstanceInitializer(InitializationFileOps ops) : ops_(ops) {}
    InitResult<std::unique_ptr<InitializationLease>> create(const std::filesystem::path& path, const InitializationIdentity& identity) override {
        auto stage = InitStage::CreateDirectory;
        try {
            auto root = detail::openInstanceRoot(path);
            if (!root) return initializationError(root.error().code, root.error().code == ErrorCode::InvalidArgument ? InitStage::Validate : stage);
            auto empty = emptyRoot(root.value().rootFd.get());
            if (!empty) return empty.error();
            if (::mkdirat(root.value().rootFd.get(), ".anof", 0700) != 0) return initializationErrno(errno, stage);
            struct stat created {};
            if (::fstatat(root.value().rootFd.get(), ".anof", &created, AT_SYMLINK_NOFOLLOW) != 0)
                return initializationErrno(errno, stage);
            if (!S_ISDIR(created.st_mode) || created.st_uid != ::geteuid() || (created.st_mode & 07777) != 0700)
                return InitFailure { InitError::IntegrityError, stage };
            // 复用路径检查并核对持有 root，避免在不同目录上获得锁。
            auto directories = detail::openInstanceDirectories(root.value().root);
            if (!directories) return initializationError(directories.error().code, stage);
            struct stat held{}, opened{};
            if (::fstat(root.value().rootFd.get(), &held) != 0 || ::fstat(directories.value().rootFd.get(), &opened) != 0) return initializationErrno(errno, stage);
            if (held.st_dev != opened.st_dev || held.st_ino != opened.st_ino) return InitFailure{ InitError::IntegrityError, stage };
            stage = InitStage::AcquireLock;
            auto locked = detail::acquireInstanceLease(std::move(directories).value(), true);
            if (!locked) return locked.error().code == ErrorCode::Conflict ? InitFailure{ InitError::Busy, stage } : initializationError(locked.error().code, stage);
            stage = InitStage::WritePending;
            auto synced = syncFile(detail::instanceLockFile(*locked.value()).value(), ops_, stage, false);
            if (!synced) return synced.error();
            const int directory = detail::instanceDirectory(*locked.value()).value();
            auto written = publish(directory, root.value().rootFd.get(), "initialization.pending", detail::pendingText(identity), identity, ops_, stage, false);
            if (!written) return written.error();
            auto pending = readFile(directory, "initialization.pending", 4096, stage);
            if (!pending) return pending.error();
            return std::unique_ptr<InitializationLease>(new LinuxInitializationLease(std::move(locked).value(), identity, pending.value().status, ops_, std::nullopt, false));
        } catch (const std::bad_alloc&) { return InitFailure{ InitError::InternalError, stage }; }
        catch (const std::filesystem::filesystem_error&) { return InitFailure{ InitError::IoError, stage }; }
    }
    InitResult<std::unique_ptr<InitializationLease>> openPending(const std::filesystem::path& path) override {
        auto stage = InitStage::AcquireLock;
        try {
            auto directories = detail::openInstanceDirectories(path);
            if (!directories) return initializationError(directories.error().code, stage);
            auto locked = detail::acquireInstanceLease(std::move(directories).value(), false);
            if (!locked) {
                if (locked.error().code == ErrorCode::Conflict) return InitFailure{ InitError::Busy, stage };
                if (locked.error().code == ErrorCode::NotFound) return InitFailure{ InitError::IncompleteInitialization, stage };
                return initializationError(locked.error().code, stage);
            }
            stage = InitStage::Verify;
            const int directory = detail::instanceDirectory(*locked.value()).value();
            auto pending = readFile(directory, "initialization.pending", 4096, stage);
            if (!pending) return pending.error().code == InitError::NotFound ? InitFailure{ InitError::IncompleteInitialization, stage } : pending.error();
            auto identity = detail::parsePending(pending.value().text, stage);
            if (!identity) return identity.error();
            auto config = configuration(directory, stage);
            if (!config) return config.error().code == InitError::NotFound ? InitFailure{ InitError::IncompleteInitialization, stage } : config.error();
            auto marker = readFile(directory, "instance.json", 4096, stage);
            if (marker) {
                auto parsed = detail::parseFormat(marker.value().text, stage);
                if (!parsed) return parsed.error();
                if (parsed.value() != identity.value().instanceId) return InitFailure{ InitError::IntegrityError, stage };
            } else if (marker.error().code != InitError::NotFound) return marker.error();
            return std::unique_ptr<InitializationLease>(new LinuxInitializationLease(std::move(locked).value(), identity.value(), pending.value().status, ops_, std::move(config).value(), true));
        } catch (const std::bad_alloc&) { return InitFailure{ InitError::InternalError, stage }; }
        catch (const std::filesystem::filesystem_error&) { return InitFailure{ InitError::IoError, stage }; }
    }
private:
    InitializationFileOps ops_;
};
}  // namespace

detail::InitializationFileOps detail::initializationFileOps() { return { ::write, ::fsync, renameNoReplace, ::unlinkat }; }
InitResult<std::unique_ptr<InstanceInitializer>> detail::makeLinuxInstanceInitializerWith(InitializationFileOps ops) {
    if (!ops.write || !ops.sync || !ops.renameNoReplace || !ops.unlink) return InitFailure{ InitError::InvalidInput, InitStage::Validate };
    try { return std::unique_ptr<InstanceInitializer>(new LinuxInstanceInitializer(ops)); }
    catch (const std::bad_alloc&) { return InitFailure{ InitError::InternalError, InitStage::Validate }; }
}
InitResult<std::unique_ptr<InstanceInitializer>> makeLinuxInstanceInitializer() {
    return detail::makeLinuxInstanceInitializerWith(detail::initializationFileOps());
}
}  // namespace anof::infrastructure::system
