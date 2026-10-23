#include <anof/infrastructure/persistence/sqlite/SqliteBlobRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
namespace anof::infrastructure::sqlite {
namespace {
constexpr const char* columns = "id,size,sha256,state,created_at,updated_at,revision,unreferenced_at,delete_started_at";
Result<BlobRecord> decode(SqliteStatement& s) {
    auto id = readId<BlobId>(s, 0);
    auto size = readQuantity<FileLength>(s, 1);
    auto digest = s.blob(2);
    auto state = s.integer(3);
    auto created = readTime(s, 4);
    auto updated = readTime(s, 5);
    auto rev = readQuantity<Revision>(s, 6);
    if (!id || !size || !digest || digest.value().size() != 32 || !state || state.value() < 0 || state.value() > 1 || !created || !updated || !rev) return ErrorCode::IntegrityError;
    std::array<std::uint8_t, 32> hash{};
    std::copy(digest.value().begin(), digest.value().end(), hash.begin());
    BlobRecord b{ { id.value(), size.value(), Sha256(hash) }, static_cast<BlobState>(state.value()), created.value(), updated.value(), rev.value(), {}, {} };
    for (int i = 7; i < 9; ++i) {
        auto nil = s.isNull(i);
        if (!nil) return nil.error();
        if (!nil.value()) {
            auto t = readTime(s, i);
            if (!t) return t.error();
            (i == 7 ? b.unreferencedAt : b.deleteStartedAt) = t.value();
        }
    }
    if (!validateBlobRecord(b)) return ErrorCode::IntegrityError;
    return b;
}
Result<void> save(SqliteTransactionContext& c, const BlobRecord& before, const BlobRecord& b) {
    return detail::one(c, detail::write(c, "UPDATE blobs SET state=?1,updated_at=?2,revision=?3,unreferenced_at=?4,delete_started_at=?5 WHERE id=?6 AND revision=?7",
                                        { std::int64_t(b.state), b.updatedAt.time_since_epoch().count(), b.revision.value(), b.unreferencedAt ? detail::Parameter(b.unreferencedAt->time_since_epoch().count()) : detail::Parameter(nullptr), b.deleteStartedAt ? detail::Parameter(b.deleteStartedAt->time_since_epoch().count()) : detail::Parameter(nullptr), b.content.id.toString(), before.revision.value() }));
}
}  // namespace
Result<void> SqliteBlobRepository::insert(const BlobRecord& b) {
    if (!validateBlobRecord(b) || b.state != BlobState::Available || b.revision.value() != 1 || b.unreferencedAt) return context_.fail(ErrorCode::IntegrityError);
    return detail::one(context_, detail::write(context_, "INSERT INTO blobs(id,size,sha256,state,created_at,updated_at,revision,unreferenced_at,delete_started_at) VALUES(?1,?2,?3,0,?4,?5,1,?6,NULL)",
                                               { b.content.id.toString(), b.content.size.value(), detail::BlobParameter{ b.content.sha256.bytes().data(), 32 }, b.createdAt.time_since_epoch().count(), b.updatedAt.time_since_epoch().count(), b.unreferencedAt ? detail::Parameter(b.unreferencedAt->time_since_epoch().count()) : detail::Parameter(nullptr) }));
}
Result<std::optional<BlobRecord>> SqliteBlobRepository::record(BlobId id) {
    auto q = detail::query(context_, "SELECT " + std::string(columns) + " FROM blobs WHERE id=?1", { id.toString() });
    if (!q) return q.error();
    auto r = q.value()->step();
    if (!r) return context_.fail(r.error());
    if (!r.value()) return std::optional<BlobRecord>{};
    auto b = decode(*q.value());
    if (!b) return context_.fail(b.error());
    return std::optional<BlobRecord>{ b.value() };
}
Result<BlobRecord> SqliteBlobRepository::markUnreferenced(BlobId id, Revision expected, UtcTime now) {
    auto old = record(id);
    if (!old) return old.error();
    if (!old.value()) return context_.fail(ErrorCode::NotFound);
    auto b = *old.value();
    if (b.revision != expected || b.state != BlobState::Available) return context_.fail(ErrorCode::Conflict);
    auto n = hasNodeReference(id);
    auto u = hasUploadReference(id);
    if (!n) return n.error();
    if (!u) return u.error();
    if (n.value() || u.value()) return context_.fail(ErrorCode::Conflict);
    if (b.unreferencedAt) return b;
    auto rev = b.revision.add(1);
    if (!rev) return context_.fail(rev.error());
    b.updatedAt = nondecreasingEventTime(now, b.createdAt, b.updatedAt);
    b.unreferencedAt = b.updatedAt;
    b.revision = rev.value();
    // 截止时间溢出不能留下无法处理的记录。
    if (!blobGcDeadline(*b.unreferencedAt)) return context_.fail(ErrorCode::IntegrityError);
    auto saved = save(context_, *old.value(), b);
    if (!saved) return saved.error();
    return b;
}
Result<BlobRecord> SqliteBlobRepository::markDeleting(BlobId id, Revision expected, UtcTime now) {
    auto old = record(id);
    if (!old) return old.error();
    if (!old.value()) return context_.fail(ErrorCode::NotFound);
    auto b = *old.value();
    if (b.revision != expected) return context_.fail(ErrorCode::Conflict);
    auto n = hasNodeReference(id);
    auto u = hasUploadReference(id);
    if (!n) return n.error();
    if (!u) return u.error();
    if (!blobGcEligible(b, now, n.value(), u.value())) return context_.fail(ErrorCode::Conflict);
    auto rev = b.revision.add(1);
    if (!rev) return context_.fail(rev.error());
    b.state = BlobState::Deleting;
    b.updatedAt = nondecreasingEventTime(now, b.createdAt, b.updatedAt);
    b.deleteStartedAt = b.updatedAt;
    b.revision = rev.value();
    auto saved = save(context_, *old.value(), b);
    if (!saved) return saved.error();
    return b;
}
Result<void> SqliteBlobRepository::eraseDeleting(BlobId id, Revision expected) {
    auto old = record(id);
    if (!old) return old.error();
    if (!old.value()) return context_.fail(ErrorCode::NotFound);
    if (old.value()->state != BlobState::Deleting || old.value()->revision != expected) return context_.fail(ErrorCode::Conflict);
    auto n = hasNodeReference(id);
    auto u = hasUploadReference(id);
    if (!n) return n.error();
    if (!u) return u.error();
    if (n.value() || u.value()) return context_.fail(ErrorCode::Conflict);
    return detail::one(context_, detail::write(context_, "DELETE FROM blobs WHERE id=?1 AND revision=?2 AND state=1", { id.toString(), expected.value() }));
}
Result<Page<BlobRecord, BlobGcPosition>> SqliteBlobRepository::gcCandidates(UtcTime now, PageSize size, std::optional<BlobGcPosition> after) {
    if (size.value() > 16) return context_.fail(ErrorCode::InvalidArgument);
    auto q = detail::query(context_, "SELECT " + std::string(columns) + " FROM blobs WHERE state=0 AND unreferenced_at<=9223371950454775807 AND unreferenced_at+86400000000<=?1 AND (?2 IS NULL OR (unreferenced_at,id)>(?2,?3)) AND NOT EXISTS(SELECT 1 FROM nodes n WHERE n.blob_id=blobs.id) AND NOT EXISTS(SELECT 1 FROM uploads u WHERE u.pending_blob_id=blobs.id AND (u.state IN (3,4) OR u.cleanup_pending=1)) ORDER BY unreferenced_at,id LIMIT ?4",
                           { now.time_since_epoch().count(), after ? detail::Parameter(after->unreferencedAt.time_since_epoch().count()) : detail::Parameter(nullptr), after ? detail::Parameter(after->id.toString()) : detail::Parameter(nullptr), size.value() + 1 });
    if (!q) return q.error();
    Page<BlobRecord, BlobGcPosition> out;
    for (;;) {
        auto r = q.value()->step();
        if (!r) return context_.fail(r.error());
        if (!r.value()) return out;
        auto b = decode(*q.value());
        if (!b) return context_.fail(b.error());
        if (out.items.size() == static_cast<std::size_t>(size.value())) {
            auto& last = out.items.back();
            out.next = BlobGcPosition{ *last.unreferencedAt, last.content.id };
            return out;
        }
        out.items.push_back(b.value());
    }
}
Result<Page<BlobRecord, BlobId>> SqliteBlobRepository::deleting(PageSize size, std::optional<BlobId> after) {
    if (size.value() > 16) return context_.fail(ErrorCode::InvalidArgument);
    auto q = detail::query(context_, "SELECT " + std::string(columns) + " FROM blobs WHERE state=1 AND (?1 IS NULL OR id>?1) ORDER BY id LIMIT ?2", { after ? detail::Parameter(after->toString()) : detail::Parameter(nullptr), size.value() + 1 });
    if (!q) return q.error();
    Page<BlobRecord, BlobId> out;
    for (;;) {
        auto r = q.value()->step();
        if (!r) return context_.fail(r.error());
        if (!r.value()) return out;
        auto b = decode(*q.value());
        if (!b) return context_.fail(b.error());
        if (out.items.size() == static_cast<std::size_t>(size.value())) {
            out.next = out.items.back().content.id;
            return out;
        }
        out.items.push_back(b.value());
    }
}
Result<GcStatistics> SqliteBlobRepository::gcStatistics() {
    auto q = detail::query(context_, "SELECT coalesce(sum(size),0),count(*),coalesce(sum(CASE WHEN state=1 THEN size ELSE 0 END),0),coalesce(sum(state=1),0) FROM blobs WHERE unreferenced_at IS NOT NULL AND NOT EXISTS(SELECT 1 FROM nodes n WHERE n.blob_id=blobs.id) AND NOT EXISTS(SELECT 1 FROM uploads u WHERE u.pending_blob_id=blobs.id AND (u.state IN (3,4) OR u.cleanup_pending=1))");
    if (!q) return q.error();
    auto r = q.value()->step();
    if (!r || !r.value()) return context_.fail(ErrorCode::IntegrityError);
    auto a = readQuantity<ByteCount>(*q.value(), 0);
    auto b = readQuantity<NodeCount>(*q.value(), 1);
    auto c = readQuantity<ByteCount>(*q.value(), 2);
    auto d = readQuantity<NodeCount>(*q.value(), 3);
    if (!a || !b || !c || !d) return context_.fail(ErrorCode::IntegrityError);
    return GcStatistics{ a.value(), b.value(), c.value(), d.value() };
}
}  // namespace anof::infrastructure::sqlite
