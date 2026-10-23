#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStoreInternal.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataTransaction.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
#include <anof/infrastructure/persistence/sqlite/tests/SqliteTestTransferBackend.h>
#include <sys/stat.h>

#include <cstdlib>
#include <stdexcept>
namespace anof::infrastructure::sqlite {
Result<void> seedTransferUpload(SqliteTransactionContext&, const Upload&);
namespace testing {
Result<void> seedTransferMetadata(SqliteConnection&, const anof::testing::MetadataSeed&);
SqliteTestTransferBackend::SqliteTestTransferBackend() {
    char pattern[] = "/tmp/anof-transfer-db-XXXXXX";
    auto* p = mkdtemp(pattern);
    if (!p) throw std::runtime_error("temporary directory failed");
    root_ = p;
}
SqliteTestTransferBackend::~SqliteTestTransferBackend() {
    std::error_code e;
    std::filesystem::remove_all(root_, e);
}
Result<void> SqliteTestTransferBackend::reset(const anof::testing::TransferSeed& seed) {
    std::error_code error;
    for (auto suffix : { "", "-wal", "-shm", "-journal" }) std::filesystem::remove(databasePath().string() + suffix, error);
    auto c = SqliteConnection::open(databasePath(), { 0 }, SqliteOpenMode::Create);
    if (!c) return c.error();
    if (chmod(databasePath().c_str(), 0600)) return ErrorCode::InternalError;
    auto schema = createSqliteSchema(*c.value());
    if (!schema) return schema.error();
    bool active = false;
    SqliteTransactionContext context(*c.value(), active);
    auto begun = context.begin();
    if (!begun) return begun.error();
    auto base = seedTransferMetadata(*c.value(), seed.metadata);
    if (!base) return base.error();
    const auto& s = seed.storage;
    auto storage = detail::one(context, detail::write(context, "INSERT INTO storage_accounting VALUES(1,?1,?2,?3,?4)", { s.budget.value(), s.retained.value(), s.reserved.value(), s.revision.value() }));
    if (!storage) return storage.error();
    for (const auto& upload : seed.uploads) {
        auto inserted = seedTransferUpload(context, upload);
        if (!inserted) return inserted.error();
    }
    auto ledger = c.value()->execute("UPDATE node_accounting SET reserved_nodes=(SELECT count(*) FROM uploads WHERE state<5)");
    if (!ledger) return ledger.error();
    return context.commit();
}
Result<std::unique_ptr<TransferMetadataStore>> SqliteTestTransferBackend::connect() {
    auto c = SqliteConnection::open(databasePath(), { 0 }, SqliteOpenMode::Inspect);
    if (!c) return c.error();
    auto valid = verifySqliteSchema(*c.value());
    if (!valid) return valid.error();
    return std::unique_ptr<TransferMetadataStore>(new detail::MetadataStoreImpl(std::move(c).value(), { 8192, 1, 1 }));
}
Result<std::vector<TransferEvent>> SqliteTestTransferBackend::readTransferAudit() {
    auto c = SqliteConnection::open(databasePath(), { 0 }, SqliteOpenMode::Inspect);
    if (!c) return c.error();
    bool active = false;
    SqliteTransactionContext context(*c.value(), active);
    auto begun = context.beginRead();
    if (!begun) return begun.error();
    auto q = detail::query(context, "SELECT kind,request_id,actor_id,upload_id,occurred_at,failure FROM transfer_audit_events ORDER BY sequence");
    if (!q) return q.error();
    std::vector<TransferEvent> out;
    for (;;) {
        auto row = q.value()->step();
        if (!row) return row.error();
        if (!row.value()) return out;
        auto& s = *q.value();
        auto kind = s.integer(0);
        auto request = readId<RequestId>(s, 1);
        auto time = readTime(s, 4);
        if (!kind || !request || !time || kind.value() < 0 || kind.value() > 6) return ErrorCode::IntegrityError;
        std::optional<UserId> actor;
        std::optional<UploadId> upload;
        std::optional<TransferFailure> failure;
        auto a = s.isNull(2);
        auto u = s.isNull(3);
        auto f = s.isNull(5);
        if (!a || !u || !f) return ErrorCode::IntegrityError;
        if (!a.value()) {
            auto id = readId<UserId>(s, 2);
            if (!id) return id.error();
            actor = id.value();
        }
        if (!u.value()) {
            auto id = readId<UploadId>(s, 3);
            if (!id) return id.error();
            upload = id.value();
        }
        if (!f.value()) {
            auto n = s.integer(5);
            if (!n || n.value() < 0 || n.value() > 11) return ErrorCode::IntegrityError;
            failure = static_cast<TransferFailure>(n.value());
        }
        out.push_back({ static_cast<TransferEventKind>(kind.value()), request.value(), actor, upload, time.value(), failure });
    }
}
}  // namespace testing
}  // namespace anof::infrastructure::sqlite
namespace anof::testing {
std::unique_ptr<TestTransferBackend> makeTestTransferBackend() { return std::make_unique<infrastructure::sqlite::testing::SqliteTestTransferBackend>(); }
}  // namespace anof::testing
