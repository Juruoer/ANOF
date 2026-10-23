#pragma once
#include <anof/domain/drive/TreePolicy.h>
#include <anof/infrastructure/persistence/sqlite/SqliteConnection.h>
#include <sqlite3.h>
namespace anof::infrastructure::sqlite::detail {
// 单连接、作用域独占；所有返回路径移除 handler，不污染后继查询。
class QueryGuard {
public:
    QueryGuard(SqliteConnection& c, QueryBudget b) : db_(c.handle()), budget_(b), start_(std::chrono::steady_clock::now()) { sqlite3_progress_handler(db_, 1000, progress, this); }
    ~QueryGuard() { sqlite3_progress_handler(db_, 0, nullptr, nullptr); }
    bool exceeded() { return limited_ || std::chrono::steady_clock::now() - start_ >= budget_.elapsed; }
    Error error(Error e) { return exceeded() ? Error{ ErrorCode::QueryLimitExceeded } : e; }

private:
    static int progress(void* p) {
        auto& self = *static_cast<QueryGuard*>(p);
        self.instructions_ += 1000;
        self.limited_ = self.instructions_ >= self.budget_.instructions || self.exceeded();
        return self.limited_;
    }
    sqlite3* db_;
    QueryBudget budget_;
    std::chrono::steady_clock::time_point start_;
    std::uint64_t instructions_ = 0;
    bool limited_ = false;
};
}  // namespace anof::infrastructure::sqlite::detail
