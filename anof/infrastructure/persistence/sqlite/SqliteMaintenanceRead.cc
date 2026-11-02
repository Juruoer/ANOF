#include <anof/infrastructure/persistence/sqlite/SqliteInstanceFiles.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMaintenanceRead.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteUploadRepository.h>
namespace anof::infrastructure::sqlite {
Result<void> validateMaintenanceUploadFacts(const Upload& u, const ResumeUpload& p, const MaintenanceActionEvidence& e) {
    if (u.id != p.id || u.generation.value() < p.generation.value() || u.cleanupPending || u.cancelRequested ||
        u.state == UploadState::Creating || u.state == UploadState::Verifying || u.state == UploadState::Committing ||
        e.files.size() != (u.pendingBlobId ? 2u : 1u)) return ErrorCode::IntegrityError;
    const MaintenanceFileFact* temporary = nullptr;
    const MaintenanceFileFact* object = nullptr;
    for (const auto& f : e.files) {
        if (f.key == MaintenanceFileKey{ u.id } && !temporary)
            temporary = &f;
        else if (u.pendingBlobId && f.key == MaintenanceFileKey{ *u.pendingBlobId } && !object)
            object = &f;
        else
            return ErrorCode::IntegrityError;
    }
    if (!temporary || (u.pendingBlobId && !object)) return ErrorCode::IntegrityError;
    if (static_cast<int>(u.state) < 5) {
        const auto* content = u.location == StagingLocation::Temporary ? temporary : object;
        if (!content || !content->file || content->file->size.value() != u.durableOffset.value()) return ErrorCode::IntegrityError;
        if (u.location == StagingLocation::Temporary ? (object && object->file.has_value()) : temporary->file.has_value()) return ErrorCode::IntegrityError;
        if (u.location == StagingLocation::Object && u.state != UploadState::Ready) return ErrorCode::IntegrityError;
        if (u.state == UploadState::Ready && (!u.actualSha256 || content->sha256 != u.actualSha256)) return ErrorCode::IntegrityError;
    } else {
        if (temporary->file) return ErrorCode::IntegrityError;
        if (u.state == UploadState::Completed && !u.resultDeletedAt && (!object || !object->file)) return ErrorCode::IntegrityError;
        if (object && object->file && (object->file->size.value() != u.original.length.value() || !u.actualSha256 || object->sha256 != u.actualSha256)) return ErrorCode::IntegrityError;
    }
    return {};
}
Result<Upload> readMaintenanceUpload(const InstanceLease& lease, UploadId id) {
    auto db = detail::openInstanceDatabase(lease, {}, false);
    if (!db) return ErrorCode::IntegrityError;
    auto schema = verifySqliteSchema(*db.value()->connection);
    if (!schema) return schema.error();
    bool active = false;
    SqliteTransactionContext ctx(*db.value()->connection, active);
    auto begun = ctx.beginRead();
    if (!begun) return begun.error();
    SqliteUploadRepository repo(ctx);
    auto value = repo.find(id);
    if (!value) return value.error();
    if (!value.value()) return ErrorCode::NotFound;
    return *value.value();
}
}  // namespace anof::infrastructure::sqlite
