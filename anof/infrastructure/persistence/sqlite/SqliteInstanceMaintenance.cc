#include <anof/infrastructure/persistence/sqlite/SqliteInstanceMaintenance.h>
#include <anof/core/Id.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMigration.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>
#include <anof/infrastructure/system/internal/BoundedFileRead.h>
#include <anof/infrastructure/system/internal/InstanceFiles.h>

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <new>

namespace anof::infrastructure::sqlite {
namespace {
namespace files = system::detail;

class FormatSax final : public nlohmann::json_sax<nlohmann::json> {
public:
    bool null() override { return false; }
    bool boolean(bool) override { return false; }
    bool number_integer(number_integer_t value) override { return version(value == 1); }
    bool number_unsigned(number_unsigned_t value) override { return version(value == 1); }
    bool number_float(number_float_t, const string_t&) override { return false; }
    bool string(string_t& value) override {
        if (field_ != 1) return false;
        auto id = Id<struct InstanceFormatTag>::parse(value);
        if (!id || id.value().toString() != value) return false;
        field_ = 0;
        return true;
    }
    bool binary(binary_t&) override { return false; }
    bool start_object(std::size_t) override {
        if (started_) return false;
        started_ = true;
        return true;
    }
    bool key(string_t& key) override {
        if (field_ != 0 || ended_) return false;
        if (key == "instance_id" && !idSeen_) {
            idSeen_ = true;
            field_ = 1;
            return true;
        }
        if (key == "format_version" && !versionSeen_) {
            versionSeen_ = true;
            field_ = 2;
            return true;
        }
        return false;
    }
    bool end_object() override {
        ended_ = started_ && idSeen_ && versionSeen_ && field_ == 0;
        return ended_;
    }
    bool start_array(std::size_t) override { return false; }
    bool end_array() override { return false; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
private:
    bool version(bool supported) {
        if (field_ != 2 || !supported) return false;
        field_ = 0;
        return true;
    }
    bool started_ = false;
    bool ended_ = false;
    bool idSeen_ = false;
    bool versionSeen_ = false;
    int field_ = 0;
};

Result<void> verifyFormat(int directory) {
    auto file = files::openPrivateFile(directory, "instance.json", false);
    if (!file) return file.error();
    auto text = files::readBoundedFile(file.value().get(), 4096, ErrorCode::IntegrityError);
    if (!text) return text.error();
    // JSON parser 默认允许 UTF-8 BOM；本实例格式明确不接受它。
    if (text.value().compare(0, 3, "\xef\xbb\xbf") == 0) return ErrorCode::IntegrityError;
    FormatSax sax;
    if (!nlohmann::json::sax_parse(text.value(), &sax)) return ErrorCode::IntegrityError;
    return {};
}

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

Result<void> sameDirectory(int held, int opened) {
    struct stat a{}, b{};
    if (::fstat(held, &a) != 0 || ::fstat(opened, &b) != 0) return files::filesystemError(errno);
    if (a.st_dev != b.st_dev || a.st_ino != b.st_ino) return ErrorCode::IntegrityError;
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

Result<void> integrity(SqliteConnection& connection) {
    auto queryOnly = connection.execute("PRAGMA query_only=ON");
    if (!queryOnly) return queryOnly.error();
    auto begun = connection.execute("BEGIN");
    if (!begun) return begun.error();
    {
        // 最多一个诊断；在 SQL 内比较，避免把诊断内容带出 SQLite 或写入日志。
        auto check = SqliteStatement::prepare(connection, "SELECT integrity_check = 'ok' FROM pragma_integrity_check(1)");
        if (!check) return check.error();
        auto row = check.value()->step();
        if (!row) return row.error();
        if (!row.value()) return ErrorCode::IntegrityError;
        auto valid = check.value()->integer(0);
        if (!valid || valid.value() != 1) return ErrorCode::IntegrityError;
        row = check.value()->step();
        if (!row) return row.error();
        if (row.value()) return ErrorCode::IntegrityError;
    }
    // 任何中途失败均由调用方的连接析构结束读事务。
    return connection.rollback();
}

class SqliteInstanceMaintenance final : public InstanceMaintenance {
public:
    Result<void> verify(const InstanceLease& lease) override {
        try {
            auto directory = files::instanceDirectory(lease);
            if (!directory) return directory.error();
            auto format = verifyFormat(directory.value());
            if (!format) return format.error();
            auto directories = files::openInstanceDirectories(lease.root());
            if (!directories) return directories.error();
            auto same = sameDirectory(directory.value(), directories.value().anofFd.get());
            if (!same) return same.error();
            // 该 fd 比 SQLite 连接活得久，不能在连接持有 POSIX 锁期间关闭它。
            auto database = files::openPrivateFile(directory.value(), "metadata.sqlite3", true);
            if (!database) return database.error();
            auto header = walHeader(database.value().get());
            if (!header) return header.error();
            auto safe = sidecars(directory.value());
            if (!safe) return safe.error();
            auto connection = openSqliteDatabase(lease.root() / ".anof/metadata.sqlite3");
            if (!connection) return connection.error();
            same = sameDatabase(directory.value(), database.value().get());
            if (!same) return same.error();
            safe = sidecars(directory.value());
            if (!safe) return safe.error();
            auto checked = integrity(*connection.value());
            if (!checked) return checked.error();
            connection.value().reset();
            safe = sidecars(directory.value());
            if (!safe) return safe.error();
            return sameDatabase(directory.value(), database.value().get());
        } catch (const std::bad_alloc&) {
            return ErrorCode::InternalError;
        } catch (const nlohmann::json::exception&) {
            return ErrorCode::IntegrityError;
        } catch (const std::filesystem::filesystem_error&) {
            return ErrorCode::InternalError;
        }
    }
};
}  // namespace

Result<std::unique_ptr<InstanceMaintenance>> makeSqliteInstanceMaintenance() {
    try {
        return std::unique_ptr<InstanceMaintenance>(new SqliteInstanceMaintenance);
    } catch (const std::bad_alloc&) {
        return ErrorCode::InternalError;
    }
}

}  // namespace anof::infrastructure::sqlite
