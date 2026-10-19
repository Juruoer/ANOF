#include <anof/infrastructure/persistence/sqlite/SqliteInstanceFiles.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>
#include <anof/infrastructure/system/internal/InitializationFiles.h>
#include <anof/infrastructure/system/internal/BoundedFileRead.h>
#include <anof/infrastructure/config/internal/ConfigurationText.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstring>
namespace anof::infrastructure::sqlite::detail {
namespace files = system::detail;
namespace {
Result<void> privateStatus(const struct stat& status) {
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink != 1 ||
        (status.st_mode & 07777) != 0600) return ErrorCode::IntegrityError;
    return {};
}

Result<void> sidecars(int directory) {
    // 只作 fstatat，SQLite 活跃期间不另行开关数据库或 SHM fd，避免干扰 POSIX 锁。
    struct stat status{};
    for (const char* name : { "metadata.sqlite3-wal", "metadata.sqlite3-shm" }) {
        if (::fstatat(directory, name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) continue;
            return files::filesystemError(errno);
        }
        auto valid = privateStatus(status);
        if (!valid) return valid.error();
    }
    if (::fstatat(directory, "metadata.sqlite3-journal", &status, AT_SYMLINK_NOFOLLOW) == 0) return ErrorCode::IntegrityError;
    if (errno != ENOENT) return files::filesystemError(errno);
    return {};
}

Result<void> sameDatabase(int directory, int held) {
    struct stat a{}, b{};
    if (::fstat(held, &a) != 0 || ::fstatat(directory, "metadata.sqlite3", &b, AT_SYMLINK_NOFOLLOW) != 0) {
        return files::filesystemError(errno);
    }
    if (a.st_dev != b.st_dev || a.st_ino != b.st_ino) return ErrorCode::IntegrityError;
    return privateStatus(b);
}

Result<void> walHeader(int fd) {
    std::array<unsigned char, 100> header{};
    std::size_t offset = 0;
    while (offset < header.size()) {
        auto count = ::pread(fd, header.data() + offset, header.size() - offset, static_cast<off_t>(offset));
        if (count < 0) {
            if (errno == EINTR) continue;
            return files::filesystemError(errno);
        }
        if (count == 0) return ErrorCode::IntegrityError;
        offset += static_cast<std::size_t>(count);
    }
    if (std::memcmp(header.data(), "SQLite format 3\0", 16) != 0 || header[18] != 2 || header[19] != 2) {
        return ErrorCode::IntegrityError;
    }
    return {};
}


InitResult<std::string> read(int directory, const char* name, std::size_t limit) {
    auto fd = files::openPrivateFile(directory, name, false);
    if (!fd) return maintenanceFailure(fd.error().code, InitStage::Verify);
    auto text = files::readBoundedFile(fd.value().get(), limit, ErrorCode::IntegrityError);
    if (!text) return maintenanceFailure(text.error().code, InitStage::Verify);
    return std::move(text).value();
}
InitResult<void> sync(int fd, InitStage stage) {
    int result;
    do { result = ::fsync(fd); } while (result != 0 && errno == EINTR);
    if (result != 0) return InitFailure{ InitError::PublicationOutcomeUnknown, stage };
    return {};
}
}
InitFailure maintenanceFailure(ErrorCode code, InitStage stage) {
    switch (code) {
    case ErrorCode::InvalidArgument: return { InitError::InvalidInput, stage };
    case ErrorCode::NotFound: return { InitError::IncompleteInitialization, stage };
    case ErrorCode::Conflict: return { InitError::AlreadyExists, stage };
    case ErrorCode::Retryable: return { InitError::Busy, stage };
    case ErrorCode::StorageFull: return { InitError::StorageFull, stage };
    case ErrorCode::CommitOutcomeUnknown: return { InitError::CommitOutcomeUnknown, stage };
    case ErrorCode::IntegrityError: return { InitError::IntegrityError, stage };
    default: return { InitError::IoError, stage };
    }
}
Error maintenanceError(InitError code) {
    switch (code) {
    case InitError::InvalidInput: return { ErrorCode::InvalidArgument };
    case InitError::NotFound: case InitError::IncompleteInitialization: return { ErrorCode::NotFound };
    case InitError::Busy: return { ErrorCode::Retryable };
    case InitError::StorageFull: return { ErrorCode::StorageFull };
    case InitError::UnsupportedFormat: case InitError::IntegrityError: return { ErrorCode::IntegrityError };
    default: return { ErrorCode::InternalError };
    }
}
InitResult<void> verifyInitializationFiles(const InstanceLease& lease, const InitializationIdentity& identity, bool creating) {
    auto safe = files::validateInstanceLease(lease);
    if (!safe) return maintenanceFailure(safe.error().code, InitStage::Verify);
    const int directory = files::instanceDirectory(lease).value();
    auto content = files::openContentDirectories(directory);
    if (!content) return maintenanceFailure(content.error().code, InitStage::Verify);
    auto pending = read(directory, "initialization.pending", 4096);
    if (!pending) return pending.error();
    auto parsed = files::parsePending(pending.value(), InitStage::Verify);
    if (!parsed) return parsed.error();
    if (parsed.value().instanceId != identity.instanceId || parsed.value().operationId != identity.operationId)
        return InitFailure{ InitError::IntegrityError, InitStage::Verify };
    auto config = read(directory, "config.toml", 65536);
    if (!config) return config.error();
    auto valid = config::detail::parseConfiguration(config.value(), true);
    if (!valid) return maintenanceFailure(valid.error().code, InitStage::Verify);
    struct stat marker{};
    if (::fstatat(directory, "instance.json", &marker, AT_SYMLINK_NOFOLLOW) == 0) {
        if (creating) return InitFailure{ InitError::AlreadyExists, InitStage::Verify };
        auto text = read(directory, "instance.json", 4096);
        if (!text) return text.error();
        auto format = files::parseFormat(text.value(), InitStage::Verify);
        if (!format) return format.error();
        if (format.value() != identity.instanceId) return InitFailure{ InitError::IntegrityError, InitStage::Verify };
    } else if (errno != ENOENT) return files::initializationErrno(errno, InitStage::Verify);
    return {};
}
InitResult<std::unique_ptr<InstanceDatabase>> openInstanceDatabase(const InstanceLease& lease, SqliteOptions options, bool create) {
    const auto stage = create ? InitStage::CreateDatabase : InitStage::Verify;
    auto safe = files::validateInstanceLease(lease);
    if (!safe) return maintenanceFailure(safe.error().code, stage);
    const int directory = files::instanceDirectory(lease).value();
    auto result = std::make_unique<InstanceDatabase>();
    if (create) {
        struct stat found{};
        for (const char* name : { "metadata.sqlite3", "metadata.sqlite3-wal", "metadata.sqlite3-shm", "metadata.sqlite3-journal" }) {
            if (::fstatat(directory, name, &found, AT_SYMLINK_NOFOLLOW) == 0) return InitFailure{ InitError::AlreadyExists, stage };
            if (errno != ENOENT) return files::initializationErrno(errno, stage);
        }
        result->file = files::OwnedFd(::openat(directory, "metadata.sqlite3", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (result->file.get() < 0) return files::initializationErrno(errno, stage);
        safe = files::validatePrivateFile(result->file.get(), true);
        if (!safe) return maintenanceFailure(safe.error().code, stage);
    } else {
        auto file = files::openPrivateFile(directory, "metadata.sqlite3", true);
        if (!file) return maintenanceFailure(file.error().code, stage);
        result->file = std::move(file).value();
        if (auto checked = walHeader(result->file.get()); !checked) return maintenanceFailure(checked.error().code, stage);
    }
    if (auto checked = sidecars(directory); !checked) return maintenanceFailure(checked.error().code, stage);
    auto connection = SqliteConnection::open(lease.root() / ".anof/metadata.sqlite3", options, create ? SqliteOpenMode::Existing : SqliteOpenMode::Inspect);
    if (!connection) return maintenanceFailure(connection.error().code, stage);
    result->connection = std::move(connection).value();
    if (auto checked = sameDatabase(directory, result->file.get()); !checked) return maintenanceFailure(checked.error().code, stage);
    if (auto checked = sidecars(directory); !checked) return maintenanceFailure(checked.error().code, stage);
    if (create) {
        if (auto made = createSqliteSchema(*result->connection); !made) return maintenanceFailure(made.error().code, stage);
    } else {
        auto table = SqliteStatement::prepare(*result->connection, "SELECT count(*) FROM sqlite_schema WHERE name='schema_info' AND type='table'");
        if (!table) return maintenanceFailure(table.error().code, stage);
        auto present = table.value()->step();
        if (!present) return maintenanceFailure(present.error().code, stage);
        if (!present.value()) return InitFailure{ InitError::IntegrityError, stage };
        auto count = table.value()->integer(0);
        if (!count || count.value() != 1) return InitFailure{ InitError::IntegrityError, stage };
        table.value().reset();
        auto version = SqliteStatement::prepare(*result->connection, "SELECT version FROM schema_info WHERE singleton=1");
        if (!version) return InitFailure{ InitError::IntegrityError, stage };
        auto row = version.value()->step();
        if (!row) return maintenanceFailure(row.error().code, stage);
        if (!row.value()) return InitFailure{ InitError::IntegrityError, stage };
        auto number = version.value()->integer(0);
        if (!number) return InitFailure{ InitError::IntegrityError, stage };
        if (number.value() != kSqliteSchemaVersion) return InitFailure{ InitError::UnsupportedFormat, stage };
        version.value().reset();
        if (auto checked = verifySqliteSchema(*result->connection); !checked) return maintenanceFailure(checked.error().code, stage);
    }
    return result;
}
InitResult<void> InstanceDatabase::close(const InstanceLease& lease, bool synchronize, InitStage stage) {
    connection.reset();
    const int directory = files::instanceDirectory(lease).value();
    if (auto checked = files::validateInstanceLease(lease); !checked) return maintenanceFailure(checked.error().code, stage);
    if (auto checked = sidecars(directory); !checked) return maintenanceFailure(checked.error().code, stage);
    if (auto checked = sameDatabase(directory, file.get()); !checked) return maintenanceFailure(checked.error().code, stage);
    if (synchronize) {
        if (auto synced = sync(file.get(), stage); !synced) return synced.error();
        for (const char* name : { "metadata.sqlite3-wal", "metadata.sqlite3-shm" }) {
            auto sidecar = files::openPrivateFile(directory, name, true);
            if (!sidecar) { if (sidecar.error().code == ErrorCode::NotFound) continue; return maintenanceFailure(sidecar.error().code, stage); }
            if (auto synced = sync(sidecar.value().get(), stage); !synced) return synced.error();
        }
        if (auto synced = sync(directory, stage); !synced) return synced.error();
        if (auto synced = sync(files::instanceRootDirectory(lease).value(), stage); !synced) return synced.error();
    }
    return {};
}
}
