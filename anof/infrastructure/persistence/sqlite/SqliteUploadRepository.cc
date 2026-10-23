#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteUploadRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>

#include <set>
namespace anof::infrastructure::sqlite {
namespace {
constexpr const char* fields = "id,user_id,client_upload_id,space_id,original_parent_id,original_name,length,expected_sha256,parent_id,name,durable_offset,state,generation,actual_sha256,pending_blob_id,staging_location,result_node_id,created_at,last_progress_at,expires_at,terminal_at,last_error,cancel_requested,cleanup_pending,result_deleted_at";
template <class T, class F>
Result<std::optional<T>> optional(SqliteStatement& s, int col, F decode) {
    auto nil = s.isNull(col);
    if (!nil) return nil.error();
    if (nil.value()) return std::optional<T>{};
    auto value = decode(s, col);
    if (!value) return value.error();
    return std::optional<T>{ std::move(value).value() };
}
Result<Sha256> digest(SqliteStatement& s, int col) {
    auto text = s.boundedTextView(col, 64);
    if (!text) return ErrorCode::IntegrityError;
    auto value = Sha256::parse(text.value());
    if (!value) return ErrorCode::IntegrityError;
    return value.value();
}
Result<TransferFailure> failure(SqliteStatement& s, int col) {
    auto value = s.integer(col);
    if (!value || value.value() < 0 || value.value() > 11) return ErrorCode::IntegrityError;
    return static_cast<TransferFailure>(value.value());
}
Result<Upload> decode(SqliteStatement& s) {
    auto id = readId<UploadId>(s, 0);
    auto user = readId<UserId>(s, 1);
    auto client = readId<OperationId>(s, 2);
    auto space = readId<SpaceId>(s, 3);
    auto parent = readId<NodeId>(s, 4);
    auto name = s.text(5);
    auto length = readQuantity<FileLength>(s, 6);
    auto expected = optional<Sha256>(s, 7, digest);
    auto target = readId<NodeId>(s, 8);
    auto targetName = s.text(9);
    auto offset = readQuantity<UploadOffset>(s, 10);
    auto state = s.integer(11);
    auto generation = readQuantity<UploadGeneration>(s, 12);
    auto actual = optional<Sha256>(s, 13, digest);
    auto blob = optional<BlobId>(s, 14, readId<BlobId>);
    auto location = s.integer(15);
    auto node = optional<NodeId>(s, 16, readId<NodeId>);
    auto created = readTime(s, 17);
    auto progress = readTime(s, 18);
    auto expires = readTime(s, 19);
    auto terminal = optional<UtcTime>(s, 20, readTime);
    std::optional<TransferFailure> error;
    auto noError = s.isNull(21);
    if (!noError) return noError.error();
    if (!noError.value()) {
        auto decodedError = failure(s, 21);
        if (!decodedError) return decodedError.error();
        error = decodedError.value();
    }
    auto cancelled = readBoolean(s, 22);
    auto cleanup = readBoolean(s, 23);
    if (!id || !user || !client || !space || !parent || !name || !length || !expected || !target || !targetName || !offset || !state || !generation || !actual || !blob || !location || !node || !created || !progress || !expires || !terminal || !cancelled || !cleanup || state.value() < 0 || state.value() > 8 || location.value() < 0 || location.value() > 1) return ErrorCode::IntegrityError;
    Upload u{ id.value(), user.value(), client.value(), { space.value(), { parent.value(), name.value() }, length.value(), expected.value() }, { target.value(), targetName.value() }, offset.value(), static_cast<UploadState>(state.value()), generation.value(), actual.value(), blob.value(), static_cast<StagingLocation>(location.value()), node.value(), created.value(), progress.value(), expires.value(), terminal.value(), error, cancelled.value(), cleanup.value() };
    auto deleted = optional<UtcTime>(s, 24, readTime);
    if (!deleted) return deleted.error();
    u.resultDeletedAt = deleted.value();
    auto valid = validateUpload(u);
    if (!valid) return valid.error();
    return u;
}
Result<std::optional<Upload>> one(SqliteTransactionContext& c, Result<std::unique_ptr<SqliteStatement>> q) {
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row) return c.fail(row.error());
    if (!row.value()) return std::optional<Upload>{};
    auto u = decode(*q.value());
    if (!u) return c.fail(u.error());
    return std::optional<Upload>{ std::move(u).value() };
}
Result<Page<Upload, UploadPosition>> page(SqliteTransactionContext& c, const std::string& filter, std::optional<UserId> user, UtcTime now, PageSize size, std::optional<UploadPosition> after) {
    auto q = detail::query(c, "SELECT " + std::string(fields) + " FROM uploads WHERE " + filter + " AND (?3 IS NULL OR (created_at,id)>(?3,?4)) ORDER BY created_at,id LIMIT ?5",
                           { user ? detail::Parameter(user->toString()) : detail::Parameter(nullptr), now.time_since_epoch().count(),
                             after ? detail::Parameter(after->createdAt.time_since_epoch().count()) : detail::Parameter(nullptr),
                             after ? detail::Parameter(after->id.toString()) : detail::Parameter(nullptr), size.value() + 1 });
    if (!q) return q.error();
    Page<Upload, UploadPosition> out;
    for (;;) {
        auto row = q.value()->step();
        if (!row) return c.fail(row.error());
        if (!row.value()) return out;
        auto u = decode(*q.value());
        if (!u) return c.fail(u.error());
        if (out.items.size() == static_cast<std::size_t>(size.value())) {
            out.next = UploadPosition{ out.items.back().createdAt, out.items.back().id };
            return out;
        }
        out.items.push_back(std::move(u).value());
    }
}
// 编码只绑定自有局部值，SQL 字段列表由内部常量生成。
Result<void> writeUpload(SqliteTransactionContext& c, const Upload& u, bool insert, UploadMatch match) {
    std::string sql;
    if (insert) {
        sql = "INSERT INTO uploads(" + std::string(fields) + ") VALUES(";
        for (int i = 1; i <= 25; ++i) sql += (i == 1 ? "" : ",") + std::string("?") + std::to_string(i);
        sql += ")";
    } else {
        sql = "UPDATE uploads SET ";
        std::string names(fields);
        std::size_t pos = 0;
        int i = 1;
        while (pos < names.size()) {
            auto end = names.find(',', pos);
            if (end == names.npos) end = names.size();
            if (i > 1) sql += ",";
            sql += names.substr(pos, end - pos) + "=?" + std::to_string(i++);
            pos = end + 1;
        }
        sql += " WHERE id=?1 AND state=?26 AND generation=?27 AND durable_offset=?28";
    }
    auto q = detail::query(c, sql);
    if (!q) return q.error();
    auto& s = *q.value();
    auto text = [&](int n, const std::string& v) { return s.bindText(n, v); };
    auto nullable = [&](int n, const auto& v) -> Result<void> { return v ? text(n, v->toString()) : s.bindNull(n); };
    const Result<void> bindings[] = { text(1, u.id.toString()), text(2, u.userId.toString()), text(3, u.clientUploadId.toString()), text(4, u.original.spaceId.toString()), text(5, u.original.target.parentId.toString()), text(6, u.original.target.name), s.bindInt64(7, u.original.length.value()), nullable(8, u.original.expectedSha256), text(9, u.target.parentId.toString()), text(10, u.target.name), s.bindInt64(11, u.durableOffset.value()), s.bindInt64(12, static_cast<int>(u.state)), s.bindInt64(13, u.generation.value()), nullable(14, u.actualSha256), nullable(15, u.pendingBlobId), s.bindInt64(16, static_cast<int>(u.location)), nullable(17, u.resultNodeId), s.bindInt64(18, u.createdAt.time_since_epoch().count()), s.bindInt64(19, u.lastProgressAt.time_since_epoch().count()), s.bindInt64(20, u.expiresAt.time_since_epoch().count()), u.terminalAt ? s.bindInt64(21, u.terminalAt->time_since_epoch().count()) : s.bindNull(21), u.lastError ? s.bindInt64(22, static_cast<int>(*u.lastError)) : s.bindNull(22), s.bindInt64(23, u.cancelRequested), s.bindInt64(24, u.cleanupPending), u.resultDeletedAt ? s.bindInt64(25, u.resultDeletedAt->time_since_epoch().count()) : s.bindNull(25) };
    for (const auto& bound : bindings)
        if (!bound) return c.fail(bound.error());
    if (!insert) {
        for (const auto& bound : { s.bindInt64(26, static_cast<int>(match.state)), s.bindInt64(27, match.generation.value()), s.bindInt64(28, match.offset.value()) })
            if (!bound) return c.fail(bound.error());
    }
    auto row = s.step();
    if (!row) return c.fail(row.error());
    auto count = detail::query(c, "SELECT changes()");
    if (!count) return count.error();
    auto r = count.value()->step();
    if (!r || !r.value()) return c.fail(ErrorCode::IntegrityError);
    auto n = count.value()->integer(0);
    if (!n || n.value() != 1) return c.fail(ErrorCode::IntegrityError);
    return {};
}
}  // namespace
// 仅由模块测试工厂调用，生产插入仍执行容量/初态检查。
Result<void> seedTransferUpload(SqliteTransactionContext& c, const Upload& u) {
    auto valid = validateUpload(u);
    if (!valid) return c.fail(valid.error());
    return writeUpload(c, u, true, { u.state, u.generation, u.durableOffset });
}
Result<std::optional<Upload>> SqliteUploadRepository::find(UploadId id) { return one(context_, detail::query(context_, "SELECT " + std::string(fields) + " FROM uploads WHERE id=?1", { id.toString() })); }
Result<std::optional<Upload>> SqliteUploadRepository::findByClientId(UserId user, OperationId id) { return one(context_, detail::query(context_, "SELECT " + std::string(fields) + " FROM uploads WHERE user_id=?1 AND client_upload_id=?2", { user.toString(), id.toString() })); }
Result<UploadCounts> SqliteUploadRepository::counts(UserId user) {
    auto q = detail::query(context_, "SELECT coalesce(sum(state<5 AND user_id=?1),0),coalesce(sum(state<5),0),coalesce(sum(user_id=?1),0),count(*) FROM uploads", { user.toString() });
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row || !row.value()) return context_.fail(ErrorCode::IntegrityError);
    std::size_t values[4]{};
    for (int i = 0; i < 4; ++i) {
        auto n = q.value()->integer(i);
        if (!n || n.value() < 0 || static_cast<std::uint64_t>(n.value()) > std::numeric_limits<std::size_t>::max()) return context_.fail(ErrorCode::IntegrityError);
        values[i] = static_cast<std::size_t>(n.value());
    }
    return UploadCounts{ values[0], values[1], values[2], values[3] };
}
Result<Page<Upload, UploadPosition>> SqliteUploadRepository::list(UserId user, PageSize size, std::optional<UploadPosition> after) { return page(context_, "user_id=?1 AND ?2 IS NOT NULL", user, {}, size, after); }
Result<Page<Upload, UploadPosition>> SqliteUploadRepository::scan(UploadScan scan, UtcTime now, PageSize size, std::optional<UploadPosition> after) {
    std::string filter;
    switch (scan) {
    case UploadScan::Active:
        filter = "state<5";
        break;
    case UploadScan::CleanupPending:
        filter = "cleanup_pending=1";
        break;
    case UploadScan::Expired:
        filter = "state<4 AND expires_at<=?2";
        break;
    case UploadScan::RetentionElapsed:
        filter = "state>=5 AND cleanup_pending=0 AND terminal_at<=9223371432054775807 AND terminal_at+604800000000<=?2";
        break;
    default:
        return context_.fail(ErrorCode::InvalidArgument);
    }
    return page(context_, "?1 IS NULL AND ?2 IS NOT NULL AND " + filter, {}, now, size, after);
}
Result<ByteCount> SqliteUploadRepository::unwrittenCommitment() {
    auto q = detail::query(context_, "SELECT coalesce(sum(length-durable_offset),0) FROM uploads WHERE state<5 OR cleanup_pending=1");
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row) return context_.fail(ErrorCode::IntegrityError);
    if (!row.value()) return context_.fail(ErrorCode::IntegrityError);
    auto result = readQuantity<ByteCount>(*q.value(), 0);
    if (!result) return context_.fail(result.error());
    return result.value();
}
Result<void> SqliteUploadRepository::insert(const Upload& u) {
    if (!validateUpload(u) || u.state != UploadState::Creating || u.generation.value() != 1 || u.cancelRequested || u.cleanupPending) return context_.fail(ErrorCode::IntegrityError);
    auto duplicate = findByClientId(u.userId, u.clientUploadId);
    if (!duplicate) return duplicate.error();
    if (duplicate.value()) return context_.fail(ErrorCode::Conflict);
    auto c = counts(u.userId);
    if (!c) return c.error();
    auto allowed = requireUploadCapacity(c.value());
    if (!allowed) return context_.fail(allowed.error());
    return writeUpload(context_, u, true, { u.state, u.generation, u.durableOffset });
}
Result<void> SqliteUploadRepository::replace(UploadId id, UploadMatch match, const Upload& u) {
    auto old = find(id);
    if (!old) return old.error();
    if (!old.value()) return context_.fail(ErrorCode::NotFound);
    const auto& before = *old.value();
    if (before.state != match.state || before.generation != match.generation || before.durableOffset != match.offset) return context_.fail(ErrorCode::Conflict);
    if (id != u.id || !validateUploadReplacement(before, u)) return context_.fail(ErrorCode::IntegrityError);
    return writeUpload(context_, u, false, match);
}
Result<std::vector<UploadId>> SqliteUploadRepository::requestUserCancellation(UserId user) {
    auto q = detail::query(context_, "SELECT " + std::string(fields) + " FROM uploads WHERE user_id=?1 AND state<5 AND cancel_requested=0 ORDER BY created_at,id LIMIT 65", { user.toString() });
    if (!q) return q.error();
    std::vector<Upload> pending;
    for (;;) {
        auto row = q.value()->step();
        if (!row) return context_.fail(row.error());
        if (!row.value()) break;
        auto u = decode(*q.value());
        if (!u) return context_.fail(u.error());
        pending.push_back(std::move(u).value());
    }
    q.value().reset();
    if (pending.size() > 64) return context_.fail(ErrorCode::IntegrityError);
    std::vector<UploadId> ids;
    for (auto u : pending) {
        auto generation = u.generation.add(1);
        if (!generation) return context_.fail(generation.error());
        auto match = UploadMatch{ u.state, u.generation, u.durableOffset };
        u.generation = generation.value();
        u.cancelRequested = true;
        auto changed = replace(u.id, match, u);
        if (!changed) return changed.error();
        ids.push_back(u.id);
    }
    return ids;
}
Result<void> SqliteUploadRepository::eraseTerminal(UploadId id, UploadGeneration expected, UtcTime now) {
    auto u = find(id);
    if (!u) return u.error();
    if (!u.value()) return context_.fail(ErrorCode::NotFound);
    if (u.value()->generation != expected || !mayForgetUpload(*u.value(), now)) return context_.fail(ErrorCode::Conflict);
    return detail::one(context_, detail::write(context_, "DELETE FROM uploads WHERE id=?1 AND generation=?2", { id.toString(), expected.value() }));
}
Result<NodeCount> SqliteUploadRepository::markResultsDeleted(const std::vector<NodeId>& ids, UtcTime now) {
    if (ids.size() > kTreeOperationLimit) return context_.fail(ErrorCode::InvalidArgument);
    std::set<NodeId> unique(ids.begin(), ids.end());
    std::int64_t count = 0;
    for (auto id : unique) {
        auto q = detail::query(context_, "SELECT " + std::string(fields) + " FROM uploads WHERE result_node_id=?1 AND state=5 AND result_deleted_at IS NULL", { id.toString() });
        if (!q) return q.error();
        std::vector<Upload> changes;
        for (;;) {
            auto row = q.value()->step();
            if (!row) return context_.fail(row.error());
            if (!row.value()) break;
            auto u = decode(*q.value());
            if (!u) return context_.fail(u.error());
            changes.push_back(u.value());
        }
        q.value().reset();
        auto node = detail::query(context_, "SELECT updated_at FROM nodes WHERE id=?1", { id.toString() });
        if (!node) return node.error();
        auto row = node.value()->step();
        if (!row) return context_.fail(row.error());
        auto at = now;
        if (row.value()) {
            auto t = readTime(*node.value(), 0);
            if (!t) return context_.fail(t.error());
            at = std::max(at, t.value());
        }
        for (auto u : changes) {
            auto match = UploadMatch{ u.state, u.generation, u.durableOffset };
            auto next = u.generation.add(1);
            if (!next) return context_.fail(next.error());
            u.generation = next.value();
            u.resultDeletedAt = nondecreasingEventTime(at, u.createdAt, *u.terminalAt);
            auto written = replace(u.id, match, u);
            if (!written) return written.error();
            ++count;
        }
    }
    return NodeCount::from(count);
}
}  // namespace anof::infrastructure::sqlite
