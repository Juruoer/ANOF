#include <anof/application/drive/MaintenanceService.h>
#include <anof/application/drive/PurgeTree.h>
namespace anof {
Result<void> MaintenanceService::expire(TrashCondition c, RequestId request) {
    auto opened = store_.beginDriveWrite();
    if (!opened) return opened.error();
    auto& tx = *opened.value();
    auto entry = tx.trash().find(c.id);
    if (!entry) return entry.error();
    if (!entry.value()) return ErrorCode::NotFound;
    if (entry.value()->revision != c.revision || !trashExpired(*entry.value(), clock_.now())) return ErrorCode::Conflict;
    auto erased = purgeTree(tx, *entry.value(), clock_.now(), request, MaintenanceActor{});
    if (!erased) return erased;
    return tx.commit();
}
Result<BlobRecord> MaintenanceService::mark(BlobId id, Revision revision, RequestId request) {
    auto opened = store_.beginDriveWrite();
    if (!opened) return opened.error();
    auto& tx = *opened.value();
    auto marked = tx.blobs().markDeleting(id, revision, clock_.now());
    if (!marked) return marked.error();
    auto audit = tx.driveAudit().append({ request, clock_.now(), MaintenanceActor{}, GcMarked{ id } });
    if (!audit) return audit.error();
    auto commit = tx.commit();
    if (!commit) return commit.error();
    return marked.value();
}
Result<void> MaintenanceService::finalize(BlobId id, Revision revision, RequestId request) {
    auto opened = store_.beginDriveWrite();
    if (!opened) return opened.error();
    auto& tx = *opened.value();
    auto row = tx.blobs().record(id);
    if (!row) return row.error();
    if (!row.value()) return ErrorCode::NotFound;
    if (row.value()->state != BlobState::Deleting || row.value()->revision != revision) return ErrorCode::Conflict;
    auto erased = tx.blobs().eraseDeleting(id, revision);
    if (!erased) return erased;
    auto released = tx.storage().releaseRetained(ByteCount::from(row.value()->content.size.value()).value());
    if (!released) return released;
    auto audit = tx.driveAudit().append({ request, clock_.now(), MaintenanceActor{}, GcFinished{ id } });
    if (!audit) return audit;
    return tx.commit();
}
}  // namespace anof
