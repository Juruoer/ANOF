#include <anof/infrastructure/persistence/sqlite/SqliteBlobRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStorageRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransferAuditRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
namespace anof::infrastructure::sqlite {
namespace {
Result<Blob> blob(SqliteStatement& s) {
    auto id = readId<BlobId>(s, 0);
    auto size = readQuantity<FileLength>(s, 1);
    auto digest = s.blob(2);
    auto key = s.text(3);
    auto state = s.integer(4);
    if (!id || !size || !digest || !key || !state || digest.value().size() != 32 || state.value() != 0) return ErrorCode::IntegrityError;
    std::array<std::uint8_t, 32> bytes{};
    std::copy(digest.value().begin(), digest.value().end(), bytes.begin());
    Blob b{ id.value(), size.value(), Sha256(bytes) };
    if (b.storageKey() != key.value()) return ErrorCode::IntegrityError;
    return b;
}
Result<bool> exists(SqliteTransactionContext& c, const std::string& sql, BlobId id) {
    auto q = detail::query(c, sql, { id.toString() });
    if (!q) return q.error();
    auto r = q.value()->step();
    if (!r) return c.fail(r.error());
    return r.value();
}
Result<void> saveStorage(SqliteTransactionContext& c, const StorageSnapshot& before, const StorageSnapshot& after) {
    if (before.revision == after.revision) return {};
    return detail::one(c, detail::write(c, "UPDATE storage_accounting SET content_budget_bytes=?1,retained_bytes=?2,reserved_bytes=?3,revision=?4 WHERE singleton=1 AND revision=?5",
                                        { after.budget.value(), after.retained.value(), after.reserved.value(), after.revision.value(), before.revision.value() }));
}
}  // namespace
Result<std::optional<Blob>> SqliteBlobRepository::find(BlobId id) {
    auto q = detail::query(context_, "SELECT id,size,sha256,storage_key,state FROM blobs WHERE id=?1", { id.toString() });
    if (!q) return q.error();
    auto r = q.value()->step();
    if (!r) return context_.fail(r.error());
    if (!r.value()) return std::optional<Blob>{};
    auto b = blob(*q.value());
    if (!b) return context_.fail(b.error());
    return std::optional<Blob>{ b.value() };
}
Result<std::optional<Blob>> SqliteBlobRepository::findByNode(NodeId id) {
    auto q = detail::query(context_, "SELECT blob_id FROM nodes WHERE id=?1 AND kind=1", { id.toString() });
    if (!q) return q.error();
    auto r = q.value()->step();
    if (!r) return context_.fail(r.error());
    if (!r.value()) return std::optional<Blob>{};
    auto b = readId<BlobId>(*q.value(), 0);
    if (!b) return context_.fail(b.error());
    auto found = find(b.value());
    if (!found) return found.error();
    if (!found.value()) return context_.fail(ErrorCode::IntegrityError);
    return found;
}
Result<std::optional<BlobId>> SqliteBlobRepository::candidateForUpload(UploadId id) {
    auto q = detail::query(context_, "SELECT pending_blob_id FROM uploads WHERE id=?1 AND state IN (3,4)", { id.toString() });
    if (!q) return q.error();
    auto r = q.value()->step();
    if (!r) return context_.fail(r.error());
    if (!r.value()) return std::optional<BlobId>{};
    auto nil = q.value()->isNull(0);
    if (!nil) return context_.fail(nil.error());
    if (nil.value()) return std::optional<BlobId>{};
    auto b = readId<BlobId>(*q.value(), 0);
    if (!b) return context_.fail(b.error());
    return std::optional<BlobId>{ b.value() };
}
Result<bool> SqliteBlobRepository::hasNodeReference(BlobId id) { return exists(context_, "SELECT 1 FROM nodes WHERE blob_id=?1 LIMIT 1", id); }
Result<bool> SqliteBlobRepository::hasUploadReference(BlobId id) { return exists(context_, "SELECT 1 FROM uploads WHERE pending_blob_id=?1 AND (state IN (3,4) OR cleanup_pending=1) LIMIT 1", id); }
Result<Page<Blob, BlobId>> SqliteBlobRepository::list(PageSize size, std::optional<BlobId> after) {
    auto q = detail::query(context_, "SELECT id,size,sha256,storage_key,state FROM blobs WHERE (?1 IS NULL OR id>?1) ORDER BY id LIMIT ?2",
                           { after ? detail::Parameter(after->toString()) : detail::Parameter(nullptr), size.value() + 1 });
    if (!q) return q.error();
    Page<Blob, BlobId> page;
    for (;;) {
        auto r = q.value()->step();
        if (!r) return context_.fail(r.error());
        if (!r.value()) return page;
        auto b = blob(*q.value());
        if (!b) return context_.fail(b.error());
        if (page.items.size() == static_cast<std::size_t>(size.value())) {
            page.next = page.items.back().id;
            return page;
        }
        page.items.push_back(b.value());
    }
}
Result<void> SqliteBlobRepository::insert(const Blob& b) {
    return detail::one(context_, detail::write(context_, "INSERT INTO blobs(id,size,sha256,state) VALUES(?1,?2,?3,0)",
                                               { b.id.toString(), b.size.value(), detail::BlobParameter{ b.sha256.bytes().data(), b.sha256.bytes().size() } }));
}
Result<StorageSnapshot> SqliteStorageRepository::get() {
    auto q = detail::query(context_, "SELECT content_budget_bytes,retained_bytes,reserved_bytes,revision FROM storage_accounting WHERE singleton=1");
    if (!q) return q.error();
    auto r = q.value()->step();
    if (!r) return context_.fail(r.error());
    if (!r.value()) return context_.fail(ErrorCode::IntegrityError);
    auto b = readQuantity<ByteCount>(*q.value(), 0);
    auto p = readQuantity<ByteCount>(*q.value(), 1);
    auto reserved = readQuantity<ByteCount>(*q.value(), 2);
    auto revision = readQuantity<Revision>(*q.value(), 3);
    if (!b || !p || !reserved || !revision || !p.value().add(reserved.value().value())) return context_.fail(ErrorCode::IntegrityError);
    return StorageSnapshot{ b.value(), p.value(), reserved.value(), revision.value() };
}
Result<StorageSnapshot> SqliteStorageRepository::setBudget(Revision expected, ByteCount budget) {
    auto before = get();
    if (!before) return before.error();
    auto after = setStorageBudget(before.value(), expected, budget);
    if (!after) return context_.fail(after.error());
    auto saved = saveStorage(context_, before.value(), after.value());
    if (!saved) return saved.error();
    return after.value();
}
Result<void> SqliteStorageRepository::reserve(ByteCount amount) {
    auto before = get();
    if (!before) return before.error();
    auto after = reserveStorage(before.value(), amount);
    if (!after) return context_.fail(after.error());
    return saveStorage(context_, before.value(), after.value());
}
Result<void> SqliteStorageRepository::releaseReservation(ByteCount amount) {
    auto before = get();
    if (!before) return before.error();
    auto after = releaseStorage(before.value(), amount);
    if (!after) return context_.fail(after.error());
    return saveStorage(context_, before.value(), after.value());
}
Result<void> SqliteStorageRepository::publishReservation(ByteCount amount) {
    auto before = get();
    if (!before) return before.error();
    auto after = publishStorage(before.value(), amount);
    if (!after) return context_.fail(after.error());
    return saveStorage(context_, before.value(), after.value());
}
Result<void> SqliteTransferAuditRepository::append(const TransferEvent& e) {
    int kind = static_cast<int>(e.kind);
    if (kind < 0 || kind > 6 || (e.failure && (static_cast<int>(*e.failure) < 0 || static_cast<int>(*e.failure) > 11)) ||
        (e.kind == TransferEventKind::StorageLimit ? (!e.actor || e.upload || e.failure) : !e.upload)) return context_.fail(ErrorCode::InvalidArgument);
    return detail::one(context_, detail::write(context_, "INSERT INTO transfer_audit_events(kind,request_id,actor_id,upload_id,occurred_at,failure) VALUES(?1,?2,?3,?4,?5,?6)",
                                               { std::int64_t(kind), e.requestId.toString(), e.actor ? detail::Parameter(e.actor->toString()) : detail::Parameter(nullptr), e.upload ? detail::Parameter(e.upload->toString()) : detail::Parameter(nullptr), e.occurredAt.time_since_epoch().count(), e.failure ? detail::Parameter(std::int64_t(*e.failure)) : detail::Parameter(nullptr) }));
}
}  // namespace anof::infrastructure::sqlite
