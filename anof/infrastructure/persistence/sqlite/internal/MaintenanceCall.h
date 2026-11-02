#pragma once
#include <anof/application/ports/OfflineInstanceAccess.h>
#include <anof/domain/drive/TreePolicy.h>
#include <anof/infrastructure/persistence/sqlite/internal/MaintenanceSql.h>
#include <sqlite3.h>

namespace anof::infrastructure::sqlite::maintenance {
// 仅存在于一次调用栈；在连接/事务仍存活时也必须解除取消借用。
class CallProgress {
public:
    CallProgress(SqliteConnection* db, const MaintenanceContext& call)
        : db_(db ? db->handle() : nullptr), cancelled_(call.cancelled), start_(std::chrono::steady_clock::now()) {
        if (db_) sqlite3_progress_handler(db_, 1000, progress, this);
    }
    ~CallProgress() { clear(); }
    CallProgress(const CallProgress&) = delete;
    CallProgress& operator=(const CallProgress&) = delete;
    void clear() {
        if (db_) sqlite3_progress_handler(db_, 0, nullptr, nullptr);
        db_ = nullptr;
    }
    bool interrupted() const { return cancelledSeen_ || limited_; }
    MaintenanceFailure error(const MaintenanceContext& call, ErrorCode e, MaintenanceStage stage) const {
        auto out = failure(call, e, stage);
        if (e != ErrorCode::CommitOutcomeUnknown && interrupted())
            out.code = cancelledSeen_ ? MaintenanceError::Cancelled : MaintenanceError::IntegrityError;
        return out;
    }

private:
    static int progress(void* data) {
        auto& self = *static_cast<CallProgress*>(data);
        self.cancelledSeen_ = self.cancelled_.load();
        self.instructions_ += 1000;
        self.limited_ = self.instructions_ >= QueryBudget{}.instructions ||
                        std::chrono::steady_clock::now() - self.start_ >= QueryBudget{}.elapsed;
        return self.interrupted();
    }
    sqlite3* db_;
    const std::atomic<bool>& cancelled_;
    std::chrono::steady_clock::time_point start_;
    std::uint64_t instructions_ = 0;
    bool cancelledSeen_ = false, limited_ = false;
};
}  // namespace anof::infrastructure::sqlite::maintenance
