#pragma once
#include <anof/infrastructure/persistence/sqlite/SqliteConnection.h>

#include <functional>

namespace anof::infrastructure::sqlite {

// 每个读快照/写事务独立保存状态；旧作用域结束后不得借连接的新事务继续访问。
class SqliteTransactionContext {
public:
    SqliteTransactionContext(SqliteConnection& connection, bool& storeActive) : connection_(connection), storeActive_(storeActive) {}
    ~SqliteTransactionContext();
    SqliteTransactionContext(const SqliteTransactionContext&) = delete;
    SqliteTransactionContext& operator=(const SqliteTransactionContext&) = delete;

    Result<void> begin();
    Result<void> beginRead();
    Result<void> requireOpen();
    // beforeCommit 仅同步调用，亦在失败清理前调用；不能保存其借用。
    Result<void> commit(const std::function<Result<void>()>& beforeCommit = {});
    Error fail(Error error);
    Error fail(ErrorCode code) { return fail(Error{ code }); }
    SqliteConnection& connection() { return connection_; }

private:
    SqliteConnection& connection_;
    bool& storeActive_;
    bool open_ = false;
    bool started_ = false;
    bool failed_ = false;
    bool readOnly_ = false;
};

}  // namespace anof::infrastructure::sqlite
