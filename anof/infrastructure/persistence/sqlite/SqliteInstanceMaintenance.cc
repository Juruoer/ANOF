#include <anof/infrastructure/persistence/sqlite/SqliteInstanceMaintenance.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceFiles.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInitializedState.h>
#include <anof/infrastructure/system/internal/InitializationFiles.h>
#include <anof/infrastructure/system/internal/BoundedFileRead.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <new>

namespace anof::infrastructure::sqlite {
namespace {
namespace files = system::detail;
class SqliteInstanceMaintenance final : public InstanceMaintenance {
public:
    explicit SqliteInstanceMaintenance(SqliteMaintenanceOptions options) : options_(options) {}
    Result<void> verify(const InstanceLease& lease) override {
        try {
            if (auto safe = files::validateInstanceLease(lease); !safe) return safe.error();
            const int directory = files::instanceDirectory(lease).value();
            struct stat pending{};
            // 任何类型的 pending 都阻止普通启动；不解释、更不删除半成品。
            if (::fstatat(directory, "initialization.pending", &pending, AT_SYMLINK_NOFOLLOW) == 0) return ErrorCode::IntegrityError;
            if (errno != ENOENT) return files::filesystemError(errno);
            auto file = files::openPrivateFile(directory, "instance.json", false);
            if (!file) return file.error();
            auto text = files::readBoundedFile(file.value().get(), 4096, ErrorCode::IntegrityError);
            if (!text) return text.error();
            auto format = files::parseFormat(text.value(), InitStage::Verify);
            if (!format) return detail::maintenanceError(format.error().code);
            auto database = detail::openInstanceDatabase(lease, options_.connection, false);
            if (!database) return detail::maintenanceError(database.error().code);
            auto checked = detail::verifyInitialized(*database.value()->connection, options_.passwordHashLimits);
            if (!checked) return checked.error().code == ErrorCode::NotFound ? Error{ ErrorCode::IntegrityError } : checked.error();
            if (checked.value().identity.instanceId != format.value()) return ErrorCode::IntegrityError;
            if (auto closed = database.value()->close(lease, false, InitStage::Verify); !closed) return detail::maintenanceError(closed.error().code);
            return {};
        } catch (const std::bad_alloc&) { return ErrorCode::InternalError; }
        catch (const std::filesystem::filesystem_error&) { return ErrorCode::InternalError; }
    }
private:
    SqliteMaintenanceOptions options_;
};
}
Result<std::unique_ptr<InstanceMaintenance>> makeSqliteInstanceMaintenance(const SqliteMaintenanceOptions& options) {
    if (!detail::validPasswordLimits(options.passwordHashLimits) || options.connection.busyTimeoutMs < 0 || options.connection.busyTimeoutMs > 5000) return ErrorCode::InvalidArgument;
    try { return std::unique_ptr<InstanceMaintenance>(new SqliteInstanceMaintenance(options)); }
    catch (const std::bad_alloc&) { return ErrorCode::InternalError; }
}
}
