#include <anof/application/transfer/UploadService.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <cstdio>
#include <cstdlib>

namespace anof {
namespace {
// 仅按需输出固定字段；不含凭据、名称、正文或数据库错误文本。
void traceFailure(const char* stage, Error error, const Upload& before, const Upload& after,
                  std::optional<UtcTime> attemptedProgress = {}) noexcept {
    const auto* enabled = std::getenv("ANOF_TRANSFER_DIAGNOSTICS");
    if (!enabled || enabled[0] != '1' || enabled[1] != '\0') return;
    const char* code = error.code == ErrorCode::IntegrityError ? "IntegrityError" :
                       error.code == ErrorCode::CommitOutcomeUnknown ? "CommitOutcomeUnknown" :
                       error.code == ErrorCode::Retryable ? "Retryable" : "Other";
    try {
        const auto id = before.id.toString();
        std::fprintf(stderr, "ANOF_TRANSFER_DIAGNOSTIC upload_id=%s stage=%s error=%s error_value=%d created_us=%lld previous_progress_us=%lld attempted_progress_us=%lld terminal_us=%lld generation=%lld offset=%lld\n",
            id.c_str(), stage, code, static_cast<int>(error.code),
            static_cast<long long>(before.createdAt.time_since_epoch().count()),
            static_cast<long long>(before.lastProgressAt.time_since_epoch().count()),
            static_cast<long long>(attemptedProgress.value_or(after.lastProgressAt).time_since_epoch().count()),
            static_cast<long long>(after.terminalAt ? after.terminalAt->time_since_epoch().count() : -1),
            static_cast<long long>(before.generation.value()), static_cast<long long>(after.durableOffset.value()));
    } catch (...) {} // 诊断分配失败不得替换原始业务错误。
}
Result<Upload> require(UploadReader& rows, UploadId id, std::optional<UserId> owner = {}) {
    auto row = rows.find(id);
    if (!row) return row.error();
    if (!row.value() || (owner && row.value()->userId != *owner)) return ErrorCode::NotFound;
    return std::move(*row.value());
}
Result<Principal> authenticate(AuthorizationReader& auth, const RequestContext& ctx, Clock& clock) {
    return auth.requireActiveSession(ctx.credential, clock.now());
}
Result<Principal> background(UserReader& users, const Upload& upload) {
    auto user = users.find(upload.userId);
    if (!user) return user.error();
    if (!user.value()) return ErrorCode::IntegrityError;
    if (!user.value()->enabled || upload.cancelRequested) return ErrorCode::UploadTerminated;
    return Principal{ user.value()->id, user.value()->role, user.value()->authEpoch };
}
Result<std::vector<QuotaScope>> scopes(SpaceReader& spaces, const Upload& u) {
    auto space = spaces.find(u.original.spaceId);
    if (!space) return space.error();
    if (!space.value()) return ErrorCode::IntegrityError;
    std::vector<QuotaScope> out{ SpaceQuota{ u.original.spaceId } };
    if (space.value()->kind == SpaceKind::Public)
        out.push_back(PublicContributionQuota{ u.original.spaceId, u.userId });
    return out;
}
Result<void> target(MetadataReadSnapshot& read, const Principal& actor, SpaceId spaceId,
                    const UploadTarget& destination) {
    auto space = read.spaces().find(spaceId);
    if (!space) return space.error();
    if (!space.value()) return ErrorCode::NotFound;
    auto permission = requireCreatePermission(actor, *space.value());
    if (!permission) return permission.error();
    if (!validateDirectoryName(destination.name)) return ErrorCode::InvalidName;
    auto path = read.nodes().findActiveDirectoryPath(spaceId, destination.parentId);
    if (!path) return path.error();
    return validateDirectoryParent(*space.value(), destination.parentId, path.value());
}
// 写事务接口与快照不互相转换，保持同一事务的显式仓储引用。
Result<void> target(TransferTransaction& tx, const Principal& actor, SpaceId spaceId, const UploadTarget& destination) {
    auto space = tx.spaces().find(spaceId);
    if (!space) return space.error();
    if (!space.value()) return ErrorCode::NotFound;
    auto permission = requireCreatePermission(actor, *space.value());
    if (!permission) return permission.error();
    if (!validateDirectoryName(destination.name)) return ErrorCode::InvalidName;
    auto path = tx.nodes().findActiveDirectoryPath(spaceId, destination.parentId);
    if (!path) return path.error();
    return validateDirectoryParent(*space.value(), destination.parentId, path.value());
}
Result<void> availableName(NodeReader& nodes, SpaceId space, const UploadTarget& target) {
    std::optional<ChildPosition> after;
    do {
        auto page = nodes.listChildren(space, target.parentId, PageSize::from(200).value(), after);
        if (!page) return page.error();
        for (const auto& item : page.value().items) {
            if (item.metadata.name == target.name) return ErrorCode::NameConflict;
            if (item.metadata.name > target.name) return {};
        }
        after = page.value().next;
    } while (after);
    return {};
}
QuotaDelta reservation(const Upload& u) {
    return { ByteCount::from(u.original.length.value()).value(), NodeCount::from(1).value() };
}
Result<void> progress(Upload& u, UtcTime now) {
    now = nondecreasingEventTime(now, u.createdAt, u.lastProgressAt);
    auto expiry = uploadExpiry(u.createdAt, now);
    if (!expiry) {
        traceFailure("progress.expiry", expiry.error(), u, u, now);
        return expiry.error();
    }
    u.lastProgressAt = now;
    u.expiresAt = expiry.value();
    return {};
}
Result<Upload> save(TransferTransaction& tx, const Upload& old, Upload next, const RequestContext* ctx = nullptr,
                    Clock* clock = nullptr) {
    auto generation = old.generation.add(1);
    if (!generation) return generation.error();
    next.generation = generation.value();
    auto changed = tx.uploads().replace(old.id, { old.state, old.generation, old.durableOffset }, next);
    if (!changed) { traceFailure("save.replace", changed.error(), old, next); return changed.error(); }
    if (ctx) {
        auto valid = authenticate(tx.authorization(), *ctx, *clock);
        if (!valid) return valid.error();
    }
    auto committed = tx.commit();
    if (!committed) { traceFailure("save.commit", committed.error(), old, next); return committed.error(); }
    return next;
}
Result<void> active(const Upload& u, UtcTime now) {
    if (isTerminal(u.state))
        return u.state == UploadState::Completed ? ErrorCode::UploadStateConflict : ErrorCode::UploadTerminated;
    if (u.cancelRequested || (u.state != UploadState::Committing && u.expiresAt <= now))
        return ErrorCode::UploadTerminated;
    return {};
}
UploadView wire(InstanceId instance, const Upload& u) {
    return { instance, u.id,
             u.clientUploadId, u.original.spaceId,
             u.target, u.original.length,
             u.durableOffset, u.original.expectedSha256,
             u.actualSha256, u.state,
             u.resultNodeId, u.createdAt,
             u.lastProgressAt, u.expiresAt,
             u.terminalAt, u.lastError };
}
Result<InstanceId> instance(SettingsReader& settings) {
    auto value = settings.get();
    if (!value) return value.error();
    if (!value.value()) return ErrorCode::IntegrityError;
    return value.value()->instanceId;
}
Result<void> audit(TransferTransaction& tx, const Upload& u, TransferEventKind kind, RequestId request, UtcTime now) {
    return tx.transferAudit().append({ kind, request, u.userId, u.id, now, u.lastError });
}
}  // namespace
Result<std::optional<Upload>> UploadService::findReplay(const RequestContext& ctx, const CreateUploadCommand& command) {
    auto read = store_.beginTransferRead();
    if (!read) return read.error();
    auto actor = authenticate(read.value()->authorization(), ctx, clock_);
    if (!actor) return actor.error();
    auto row = read.value()->uploads().findByClientId(actor.value().userId, command.clientUploadId);
    if (!row) return row.error();
    if (!row.value()) return std::optional<Upload>{};
    if (!(row.value()->original == command.parameters)) return ErrorCode::UploadKeyConflict;
    const auto id = row.value()->id;
    read.value().reset();
    auto current = refresh(ctx, id);
    if (!current) return current.error();
    return std::optional<Upload>{ current.value() };
}
Result<UploadCreation> UploadService::create(const RequestContext& ctx, const CreateUploadCommand& command,
                                             ByteCount free, ByteCount margin) {
    auto begun = store_.beginTransferWrite();
    if (!begun) return begun.error();
    auto& tx = *begun.value();
    auto actor = authenticate(tx.authorization(), ctx, clock_);
    if (!actor) return actor.error();
    auto existing = tx.uploads().findByClientId(actor.value().userId, command.clientUploadId);
    if (!existing) return existing.error();
    if (existing.value()) {
        if (!(existing.value()->original == command.parameters)) return ErrorCode::UploadKeyConflict;
        const auto existingId = existing.value()->id;
        begun.value().reset();
        auto refreshed = refresh(ctx, existingId);
        if (!refreshed) return refreshed.error();
        return UploadCreation{ refreshed.value(), false };
    }
    auto id = ids_.uploadId();
    if (!id) return id.error();
    auto valid = target(tx, actor.value(), command.parameters.spaceId, command.parameters.target);
    if (!valid) return valid.error();
    auto now = clock_.now();
    auto expiry = uploadExpiry(now, now);
    if (!expiry) return expiry.error();
    Upload u{ id.value(),
              actor.value().userId,
              command.clientUploadId,
              command.parameters,
              command.parameters.target,
              {},
              UploadState::Creating,
              {},
              {},
              {},
              StagingLocation::Temporary,
              {},
              now,
              now,
              expiry.value(),
              {},
              {} };
    auto unwritten = tx.uploads().unwrittenCommitment();
    if (!unwritten) return unwritten.error();
    auto capacity = requireDiskCapacity(free, unwritten.value(), u.original.length, margin);
    if (!capacity) return capacity.error();
    auto charge = scopes(tx.spaces(), u);
    if (!charge) return charge.error();
    for (const auto& scope : charge.value()) {
        auto r = tx.quotas().reserve(scope, reservation(u));
        if (!r) return r.error();
    }
    auto reserved = tx.storage().reserve(reservation(u).bytes);
    if (!reserved) return reserved.error();
    auto inserted = tx.uploads().insert(u);
    if (!inserted) return inserted.error();
    auto event = audit(tx, u, TransferEventKind::Create, ctx.requestId, now);
    if (!event) return event.error();
    auto final = authenticate(tx.authorization(), ctx, clock_);
    if (!final) return final.error();
    auto committed = tx.commit();
    if (!committed) return committed.error();
    return UploadCreation{ u, true };
}
Result<Upload> UploadService::read(UploadId id) {
    auto read = store_.beginTransferRead();
    if (!read) return read.error();
    return require(read.value()->uploads(), id);
}
Result<UploadView> UploadService::view(const Upload& u) {
    auto read = store_.beginTransferRead();
    if (!read) return read.error();
    auto id = instance(read.value()->settings());
    if (!id) return id.error();
    return wire(id.value(), u);
}
Result<Upload> UploadService::refresh(const RequestContext& ctx, UploadId id) {
    auto begun = store_.beginTransferWrite();
    if (!begun) return begun.error();
    auto& tx = *begun.value();
    auto actor = authenticate(tx.authorization(), ctx, clock_);
    if (!actor) return actor.error();
    auto old = require(tx.uploads(), id, actor.value().userId);
    if (!old) return old.error();
    auto u = old.value();
    if (isTerminal(u.state) || u.state == UploadState::Committing || (!u.cancelRequested && u.expiresAt > clock_.now()))
        return u;
    u.state = u.cancelRequested ? UploadState::Cancelled : UploadState::Expired;
    u.lastError = u.cancelRequested ? TransferFailure::UserDisabled : TransferFailure::UploadExpired;
    u.terminalAt = nondecreasingEventTime(clock_.now(), u.createdAt, u.lastProgressAt);
    u.cleanupPending = true;
    auto charge = scopes(tx.spaces(), u);
    if (!charge) return charge.error();
    for (const auto& scope : charge.value()) {
        auto released = tx.quotas().releaseReservation(scope, reservation(u));
        if (!released) return released.error();
    }
    auto event = audit(tx, u, u.cancelRequested ? TransferEventKind::Cancel : TransferEventKind::Expire, ctx.requestId,
                       clock_.now());
    if (!event) return event.error();
    return save(tx, old.value(), u, &ctx, &clock_);
}
Result<UploadView> UploadService::get(const RequestContext& ctx, UploadId id) {
    auto refreshed = refresh(ctx, id);
    if (!refreshed) return refreshed.error();
    auto read = store_.beginTransferRead();
    if (!read) return read.error();
    auto actor = authenticate(read.value()->authorization(), ctx, clock_);
    if (!actor) return actor.error();
    auto u = require(read.value()->uploads(), id, actor.value().userId);
    if (!u) return u.error();
    auto identity = instance(read.value()->settings());
    if (!identity) return identity.error();
    return wire(identity.value(), u.value());
}
Result<Page<UploadView, UploadCursor>> UploadService::list(const RequestContext& ctx,
                                                           const ListUploadsCommand& command) {
    auto read = store_.beginTransferRead();
    if (!read) return read.error();
    auto& r = *read.value();
    auto actor = authenticate(r.authorization(), ctx, clock_);
    if (!actor) return actor.error();
    if ((command.after && command.clientUploadId) || (command.after && command.after->userId != actor.value().userId))
        return ErrorCode::InvalidArgument;
    auto identity = instance(r.settings());
    if (!identity) return identity.error();
    Page<UploadView, UploadCursor> out;
    if (command.clientUploadId) {
        auto u = r.uploads().findByClientId(actor.value().userId, *command.clientUploadId);
        if (!u) return u.error();
        const auto selected = u.value();
        read.value().reset();
        if (selected) {
            auto refreshed = get(ctx, selected->id);
            if (!refreshed) return refreshed.error();
            out.items.push_back(refreshed.value());
        }
        return out;
    }
    auto page = r.uploads().list(actor.value().userId, command.size,
                                 command.after ? std::optional<UploadPosition>(command.after->position) : std::nullopt);
    if (!page) return page.error();
    read.value().reset();
    for (const auto& u : page.value().items) {
        auto refreshed = get(ctx, u.id);
        if (!refreshed) return refreshed.error();
        out.items.push_back(refreshed.value());
    }
    if (page.value().next) out.next = UploadCursor{ actor.value().userId, *page.value().next };
    return out;
}
Result<Upload> UploadService::opened(UploadId id, UploadGeneration generation) {
    auto begun = store_.beginTransferWrite();
    if (!begun) return begun.error();
    auto& tx = *begun.value();
    auto old = require(tx.uploads(), id);
    if (!old) return old.error();
    if (old.value().generation != generation || old.value().state != UploadState::Creating) return ErrorCode::Conflict;
    auto allowed = active(old.value(), clock_.now());
    if (!allowed) return allowed.error();
    auto u = old.value();
    u.state = UploadState::Open;
    return save(tx, old.value(), u);
}
Result<UploadPatch> UploadService::preparePatch(const RequestContext& ctx, const PreparePatchCommand& command) {
    auto refreshed = refresh(ctx, command.id);
    if (!refreshed) return refreshed.error();
    auto read = store_.beginTransferRead();
    if (!read) return read.error();
    auto& r = *read.value();
    auto actor = authenticate(r.authorization(), ctx, clock_);
    if (!actor) return actor.error();
    auto u = require(r.uploads(), command.id, actor.value().userId);
    if (!u) return u.error();
    auto allowed = active(u.value(), clock_.now());
    if (!allowed) return allowed.error();
    if (u.value().state != UploadState::Open) return ErrorCode::UploadStateConflict;
    auto valid = target(r, actor.value(), u.value().original.spaceId, u.value().target);
    if (!valid) return valid.error();
    if (command.offset != u.value().durableOffset) return ErrorCode::UploadOffsetMismatch;
    if (command.length.value() < 1 || command.length.value() > kMaxPatchBytes) return ErrorCode::InvalidArgument;
    auto end = checkedUploadEnd(u.value().original.length, command.offset, command.length);
    if (!end) return end.error();
    return UploadPatch{ u.value(), end.value() };
}
Result<Upload> UploadService::commitPatch(const RequestContext& ctx, const UploadPatch& patch) {
    auto begun = store_.beginTransferWrite();
    if (!begun) return begun.error();
    auto& tx = *begun.value();
    auto actor = authenticate(tx.authorization(), ctx, clock_);
    if (!actor) return actor.error();
    auto old = require(tx.uploads(), patch.upload.id, actor.value().userId);
    if (!old) return old.error();
    if (old.value().generation != patch.upload.generation || old.value().durableOffset != patch.upload.durableOffset ||
        old.value().state != UploadState::Open)
        return ErrorCode::Conflict;
    auto allowed = active(old.value(), clock_.now());
    if (!allowed) return allowed.error();
    auto valid = target(tx, actor.value(), old.value().original.spaceId, old.value().target);
    if (!valid) return valid.error();
    if (patch.end.value() <= old.value().durableOffset.value() ||
        patch.end.value() > old.value().original.length.value())
        return ErrorCode::InvalidArgument;
    auto next = old.value();
    next.durableOffset = patch.end;
    auto moved = progress(next, clock_.now());
    if (!moved) return moved.error();
    return save(tx, old.value(), next, &ctx, &clock_);
}
Result<Upload> UploadService::acceptComplete(const RequestContext& ctx, const CompleteUploadCommand& command) {
    auto refreshed = refresh(ctx, command.id);
    if (!refreshed) return refreshed.error();
    auto begun = store_.beginTransferWrite();
    if (!begun) return begun.error();
    auto& tx = *begun.value();
    auto actor = authenticate(tx.authorization(), ctx, clock_);
    if (!actor) return actor.error();
    auto old = require(tx.uploads(), command.id, actor.value().userId);
    if (!old) return old.error();
    auto u = old.value();
    if (u.state == UploadState::Completed || u.state == UploadState::Verifying || u.state == UploadState::Committing) {
        if (command.target && !(*command.target == u.target)) return ErrorCode::UploadStateConflict;
        if (u.state != UploadState::Completed) {
            auto allowed = active(u, clock_.now());
            if (!allowed) return allowed.error();
        }
        return u;
    }
    auto allowed = active(u, clock_.now());
    if (!allowed) return allowed.error();
    if (u.state != UploadState::Open && u.state != UploadState::Ready) return ErrorCode::UploadStateConflict;
    if (u.durableOffset.value() != u.original.length.value()) return ErrorCode::UploadIncomplete;
    if (command.target) u.target = *command.target;
    auto valid = target(tx, actor.value(), u.original.spaceId, u.target);
    if (!valid) return valid.error();
    if (u.state == UploadState::Ready) {
        auto name = availableName(tx.nodes(), u.original.spaceId, u.target);
        if (!name) return name.error();
    }
    if (u.state == UploadState::Open || !(u.target == old.value().target)) {
        auto moved = progress(u, clock_.now());
        if (!moved) return moved.error();
    }
    if (u.state == UploadState::Open) u.state = UploadState::Verifying;
    u.lastError.reset();
    auto event = audit(tx, u, TransferEventKind::AcceptComplete, ctx.requestId, clock_.now());
    if (!event) return event.error();
    return save(tx, old.value(), u, &ctx, &clock_);
}
Result<Upload> UploadService::recordVerification(UploadId id, UploadGeneration generation, Sha256 digest,
                                                 RequestId request) {
    auto begun = store_.beginTransferWrite();
    if (!begun) return begun.error();
    auto& tx = *begun.value();
    auto old = require(tx.uploads(), id);
    if (!old) return old.error();
    if (old.value().generation != generation || old.value().state != UploadState::Verifying) return ErrorCode::Conflict;
    auto enabled = background(tx.users(), old.value());
    if (!enabled) return enabled.error();
    auto allowed = active(old.value(), clock_.now());
    if (!allowed) return allowed.error();
    auto u = old.value();
    u.actualSha256 = digest;
    if (u.original.expectedSha256 && *u.original.expectedSha256 != digest) {
        u.state = UploadState::Failed;
        u.lastError = TransferFailure::ChecksumMismatch;
        u.terminalAt = nondecreasingEventTime(clock_.now(), u.createdAt, u.lastProgressAt);
        u.cleanupPending = true;
        auto charge = scopes(tx.spaces(), u);
        if (!charge) return charge.error();
        for (const auto& scope : charge.value()) {
            auto r = tx.quotas().releaseReservation(scope, reservation(u));
            if (!r) return r.error();
        }
        auto event = audit(tx, u, TransferEventKind::Fail, request, clock_.now());
        if (!event) return event.error();
    } else
        u.state = UploadState::Ready;
    return save(tx, old.value(), u);
}
Result<Upload> UploadService::preparePublish(UploadId id, UploadGeneration generation) {
    auto candidate = ids_.blobId();
    if (!candidate) return candidate.error();
    auto begun = store_.beginTransferWrite();
    if (!begun) return begun.error();
    auto& tx = *begun.value();
    auto old = require(tx.uploads(), id);
    if (!old) return old.error();
    if (old.value().generation != generation || old.value().state != UploadState::Ready) return ErrorCode::Conflict;
    auto actor = background(tx.users(), old.value());
    if (!actor) return actor.error();
    auto allowed = active(old.value(), clock_.now());
    if (!allowed) return allowed.error();
    auto valid = target(tx, actor.value(), old.value().original.spaceId, old.value().target);
    if (valid) valid = availableName(tx.nodes(), old.value().original.spaceId, old.value().target);
    if (!valid) {
        const auto code = valid.error().code;
        if (code == ErrorCode::NameConflict || code == ErrorCode::NotFound || code == ErrorCode::ParentNotDirectory ||
            code == ErrorCode::DepthLimitExceeded) {
            auto u = old.value();
            u.lastError = code == ErrorCode::NameConflict         ? TransferFailure::NameConflict
                          : code == ErrorCode::ParentNotDirectory ? TransferFailure::ParentNotDirectory
                          : code == ErrorCode::DepthLimitExceeded ? TransferFailure::DepthLimitExceeded
                                                                  : TransferFailure::NotFound;
            auto saved = save(tx, old.value(), u);
            if (!saved) return saved.error();
        }
        return valid.error();
    }
    auto u = old.value();
    u.state = UploadState::Committing;
    if (!u.pendingBlobId) u.pendingBlobId = candidate.value();
    return save(tx, old.value(), u);
}
Result<Upload> UploadService::commitPublish(UploadId id, UploadGeneration generation, RequestId request) {
    auto node = nodes_.next();
    if (!node) return node.error();
    auto begun = store_.beginTransferWrite();
    if (!begun) return begun.error();
    auto& tx = *begun.value();
    auto old = require(tx.uploads(), id);
    if (!old) return old.error();
    if (old.value().state == UploadState::Completed) return old.value();
    if (old.value().generation != generation || old.value().state != UploadState::Committing ||
        !old.value().pendingBlobId || !old.value().actualSha256)
        return ErrorCode::Conflict;
    auto actor = background(tx.users(), old.value());
    if (!actor) return actor.error();
    auto u = old.value();
    auto valid = target(tx, actor.value(), u.original.spaceId, u.target);
    if (!valid) return valid.error();
    const auto publishedAt = nondecreasingEventTime(clock_.now(), u.createdAt, u.lastProgressAt);
    auto inserted = tx.blobs().insert({ *u.pendingBlobId, u.original.length, *u.actualSha256 });
    if (!inserted) return inserted.error();
    auto file = tx.nodes().insertFile(
        { { node.value(), u.original.spaceId, u.target.parentId, u.target.name, u.userId, publishedAt },
          *u.pendingBlobId });
    if (!file) return file.error();
    auto parent = tx.nodes().find(u.target.parentId);
    if (!parent) return parent.error();
    if (!parent.value()) return ErrorCode::IntegrityError;
    auto rev = tx.nodes().advanceRevision(u.target.parentId, parent.value()->metadata.revision, clock_.now());
    if (!rev) return rev.error();
    auto charge = scopes(tx.spaces(), u);
    if (!charge) return charge.error();
    for (const auto& scope : charge.value()) {
        auto r = tx.quotas().commitReservation(scope, reservation(u));
        if (!r) return r.error();
    }
    auto accounting = tx.storage().publishReservation(reservation(u).bytes);
    if (!accounting) return accounting.error();
    u.state = UploadState::Completed;
    u.location = StagingLocation::Object;
    u.resultNodeId = node.value();
    u.terminalAt = publishedAt;
    u.lastError.reset();
    auto event = audit(tx, u, TransferEventKind::Publish, request, clock_.now());
    if (!event) return event.error();
    return save(tx, old.value(), u);
}
Result<Upload> UploadService::markObject(UploadId id, UploadGeneration generation) {
    auto begun = store_.beginTransferWrite();
    if (!begun) return begun.error();
    auto& tx = *begun.value();
    auto old = require(tx.uploads(), id);
    if (!old) return old.error();
    if (old.value().generation != generation || old.value().state != UploadState::Committing)
        return ErrorCode::Conflict;
    if (old.value().location == StagingLocation::Object) return old.value();
    auto u = old.value();
    u.location = StagingLocation::Object;
    return save(tx, old.value(), u);
}
Result<Upload> UploadService::publicationConflict(UploadId id, UploadGeneration generation, TransferFailure failure) {
    auto begun = store_.beginTransferWrite();
    if (!begun) return begun.error();
    auto& tx = *begun.value();
    auto old = require(tx.uploads(), id);
    if (!old) return old.error();
    if (old.value().generation != generation || old.value().state != UploadState::Committing)
        return ErrorCode::Conflict;
    auto u = old.value();
    u.state = UploadState::Ready;
    u.location = StagingLocation::Object;
    u.lastError = failure;
    return save(tx, old.value(), u);
}
Result<Upload> UploadService::cancel(const RequestContext& ctx, UploadId id) {
    auto begun = store_.beginTransferWrite();
    if (!begun) return begun.error();
    auto& tx = *begun.value();
    auto actor = authenticate(tx.authorization(), ctx, clock_);
    if (!actor) return actor.error();
    auto old = require(tx.uploads(), id, actor.value().userId);
    if (!old) return old.error();
    if (old.value().state == UploadState::Committing) return ErrorCode::UploadCommitInProgress;
    if (old.value().state == UploadState::Completed) return ErrorCode::UploadStateConflict;
    if (isTerminal(old.value().state)) return old.value();
    auto u = old.value();
    u.state = UploadState::Cancelled;
    u.cleanupPending = true;
    u.terminalAt = nondecreasingEventTime(clock_.now(), u.createdAt, u.lastProgressAt);
    u.lastError = TransferFailure::UploadCancelled;
    auto charge = scopes(tx.spaces(), u);
    if (!charge) return charge.error();
    for (const auto& scope : charge.value()) {
        auto r = tx.quotas().releaseReservation(scope, reservation(u));
        if (!r) return r.error();
    }
    auto event = audit(tx, u, TransferEventKind::Cancel, ctx.requestId, clock_.now());
    if (!event) return event.error();
    return save(tx, old.value(), u, &ctx, &clock_);
}
Result<Upload> UploadService::terminate(UploadId id, UploadGeneration generation, UploadState state,
                                        TransferFailure failure, RequestId request) {
    if (state != UploadState::Cancelled && state != UploadState::Expired && state != UploadState::Failed)
        return ErrorCode::InvalidArgument;
    auto begun = store_.beginTransferWrite();
    if (!begun) return begun.error();
    auto& tx = *begun.value();
    auto old = require(tx.uploads(), id);
    if (!old) return old.error();
    if (old.value().generation != generation) return ErrorCode::Conflict;
    if (isTerminal(old.value().state)) return old.value();
    auto u = old.value();
    u.state = state;
    u.cleanupPending = true;
    u.terminalAt = nondecreasingEventTime(clock_.now(), u.createdAt, u.lastProgressAt);
    u.lastError = failure;
    auto charge = scopes(tx.spaces(), u);
    if (!charge) return charge.error();
    for (const auto& scope : charge.value()) {
        auto r = tx.quotas().releaseReservation(scope, reservation(u));
        if (!r) return r.error();
    }
    auto event = audit(tx, u,
                       state == UploadState::Expired     ? TransferEventKind::Expire
                       : state == UploadState::Cancelled ? TransferEventKind::Cancel
                                                         : TransferEventKind::Fail,
                       request, clock_.now());
    if (!event) return event.error();
    return save(tx, old.value(), u);
}
Result<Upload> UploadService::confirmCleanup(UploadId id, UploadGeneration generation) {
    auto begun = store_.beginTransferWrite();
    if (!begun) return begun.error();
    auto& tx = *begun.value();
    auto old = require(tx.uploads(), id);
    if (!old) return old.error();
    if (old.value().generation != generation || !isTerminal(old.value().state)) return ErrorCode::Conflict;
    if (!old.value().cleanupPending) return old.value();
    auto released = tx.storage().releaseReservation(reservation(old.value()).bytes);
    if (!released) return released.error();
    auto u = old.value();
    u.cleanupPending = false;
    return save(tx, old.value(), u);
}
Result<StorageView> UploadService::storage(const RequestContext& ctx) {
    auto read = store_.beginTransferRead();
    if (!read) return read.error();
    auto actor = authenticate(read.value()->authorization(), ctx, clock_);
    if (!actor) return actor.error();
    if (actor.value().role != UserRole::Administrator) return ErrorCode::Forbidden;
    auto id = instance(read.value()->settings());
    if (!id) return id.error();
    auto accounting = read.value()->storage().get();
    if (!accounting) return accounting.error();
    return StorageView{ id.value(), accounting.value(), {} };
}
Result<StorageView> UploadService::setBudget(const RequestContext& ctx, const SetStorageBudgetCommand& command) {
    auto begun = store_.beginTransferWrite();
    if (!begun) return begun.error();
    auto& tx = *begun.value();
    auto actor = authenticate(tx.authorization(), ctx, clock_);
    if (!actor) return actor.error();
    if (actor.value().role != UserRole::Administrator) return ErrorCode::Forbidden;
    auto id = instance(tx.settings());
    if (!id) return id.error();
    if (command.condition.instance != id.value()) return ErrorCode::PreconditionFailed;
    auto before = tx.storage().get();
    if (!before) return before.error();
    auto changed = tx.storage().setBudget(command.condition.revision, command.budget);
    if (!changed) return changed.error();
    if (changed.value().revision != before.value().revision) {
        auto event = tx.transferAudit().append(
            { TransferEventKind::StorageLimit, ctx.requestId, actor.value().userId, {}, clock_.now(), {} });
        if (!event) return event.error();
    }
    auto final = authenticate(tx.authorization(), ctx, clock_);
    if (!final) return final.error();
    auto committed = tx.commit();
    if (!committed) return committed.error();
    return StorageView{ id.value(), changed.value(), {} };
}
}  // namespace anof
