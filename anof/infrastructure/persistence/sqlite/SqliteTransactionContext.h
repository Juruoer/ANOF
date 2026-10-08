#pragma once

#include <anof/infrastructure/persistence/sqlite/SqliteConnection.h>

namespace anof::infrastructure::sqlite {

// 每个事务独立保存状态；旧事务结束后不得借连接的新事务继续读写。
class SqliteTransactionContext {
public:
    SqliteTransactionContext(SqliteConnection& connection, bool& storeActive) : connection_(connection), storeActive_(storeActive) {}
    ~SqliteTransactionContext();
    SqliteTransactionContext(const SqliteTransactionContext&) = delete;
    SqliteTransactionContext& operator=(const SqliteTransactionContext&) = delete;

    Result<void> begin();
    Result<void> requireOpen();
    Result<void> commit();
    Error fail(Error error);
    Error fail(ErrorCode code) { return fail(Error{ code }); }
    SqliteConnection& connection() { return connection_; }

private:
    SqliteConnection& connection_;
    bool& storeActive_;
    bool open_ = false;
    bool started_ = false;
    bool failed_ = false;
};

}  // namespace anof::infrastructure::sqlite
