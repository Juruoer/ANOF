#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataTransaction.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMigration.h>

namespace anof::infrastructure::sqlite {
namespace {

class SqliteMetadataStore final : public MetadataStore {
public:
    explicit SqliteMetadataStore(std::unique_ptr<SqliteConnection> connection) : connection_(std::move(connection)) {}
    Result<std::unique_ptr<MetadataTransaction>> beginWrite() override {
        if (active_) return ErrorCode::InvalidArgument;
        // 所有可能分配内存的构造先完成；BEGIN 后的失败路径由事务上下文回滚。
        auto transaction = std::make_unique<SqliteMetadataTransaction>(*connection_, active_);
        auto begun = transaction->begin();
        if (!begun) return begun.error();
        return std::unique_ptr<MetadataTransaction>(std::move(transaction));
    }
private:
    std::unique_ptr<SqliteConnection> connection_;
    bool active_ = false;
};

}  // namespace

Result<std::unique_ptr<MetadataStore>> openSqliteMetadataStore(const std::filesystem::path& path, SqliteOptions options) {
    auto connection = openSqliteDatabase(path, options);
    if (!connection) return connection.error();
    return std::unique_ptr<MetadataStore>(new SqliteMetadataStore(std::move(connection).value()));
}

}  // namespace anof::infrastructure::sqlite
