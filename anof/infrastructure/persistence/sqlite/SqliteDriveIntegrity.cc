#include <anof/infrastructure/persistence/sqlite/SqliteDriveIntegrity.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>
namespace anof::infrastructure::sqlite {
Result<void> verifyDriveMembership(SqliteConnection& c) {
    auto q = SqliteStatement::prepare(c,
                                      "SELECT NOT EXISTS(SELECT 1 FROM nodes n LEFT JOIN trash_entries t ON t.id=n.trash_entry_id WHERE "
                                      "(n.trashed=1 AND (t.id IS NULL OR t.space_id<>n.space_id OR n.updated_at<t.deleted_at OR (n.id=t.root_node_id AND (n.parent_id IS NOT NULL OR n.name<>t.original_name)) OR "
                                      "(n.id<>t.root_node_id AND NOT EXISTS(SELECT 1 FROM nodes p WHERE p.id=n.parent_id AND p.trash_entry_id=n.trash_entry_id AND p.kind=0)))) OR "
                                      "(n.trashed=0 AND (n.trash_entry_id IS NOT NULL OR (n.parent_id IS NOT NULL AND NOT EXISTS(SELECT 1 FROM nodes p WHERE p.id=n.parent_id AND p.trashed=0 AND p.kind=0))))) "
                                      "AND NOT EXISTS(SELECT 1 FROM trash_entries t WHERE "
                                      "NOT EXISTS(SELECT 1 FROM nodes n WHERE n.id=t.root_node_id AND n.trash_entry_id=t.id AND n.parent_id IS NULL AND n.kind=t.kind) OR "
                                      "t.node_count<>(SELECT count(*) FROM nodes n WHERE n.trash_entry_id=t.id) OR "
                                      "t.logical_bytes<>(SELECT coalesce(sum(b.size),0) FROM nodes n LEFT JOIN blobs b ON n.blob_id=b.id WHERE n.trash_entry_id=t.id) OR "
                                      "t.sole_creator_id IS NOT (SELECT CASE WHEN count(DISTINCT created_by)=1 THEN min(created_by) ELSE NULL END FROM nodes WHERE trash_entry_id=t.id))");
    if (!q) return q.error();
    auto r = q.value()->step();
    if (!r) return r.error();
    if (!r.value()) return ErrorCode::IntegrityError;
    auto ok = q.value()->integer(0);
    if (!ok || ok.value() != 1) return ErrorCode::IntegrityError;
    // 每个条目最多1000成员；从根遍历核对可达数，拒绝脱离根的环。
    auto reach = SqliteStatement::prepare(c,
                                          "WITH RECURSIVE reached(entry,id,depth) AS (SELECT id,root_node_id,0 FROM trash_entries UNION ALL SELECT r.entry,n.id,r.depth+1 FROM reached r JOIN nodes n ON n.parent_id=r.id AND n.trash_entry_id=r.entry WHERE r.depth<128) "
                                          "SELECT NOT EXISTS(SELECT 1 FROM trash_entries t WHERE t.node_count<>(SELECT count(*) FROM reached r WHERE r.entry=t.id))");
    if (!reach) return reach.error();
    r = reach.value()->step();
    if (!r) return r.error();
    if (!r.value()) return ErrorCode::IntegrityError;
    ok = reach.value()->integer(0);
    if (!ok || ok.value() != 1) return ErrorCode::IntegrityError;
    auto evidence = SqliteStatement::prepare(c,
                                             "SELECT NOT EXISTS(SELECT 1 FROM blobs b WHERE b.state=1 AND (EXISTS(SELECT 1 FROM nodes n WHERE n.blob_id=b.id) OR EXISTS(SELECT 1 FROM uploads u WHERE u.pending_blob_id=b.id AND (u.state IN (3,4) OR u.cleanup_pending=1)))) AND NOT EXISTS(SELECT 1 FROM uploads u LEFT JOIN nodes n ON n.id=u.result_node_id LEFT JOIN blobs b ON b.id=n.blob_id WHERE u.state=5 AND "
                                             "((u.result_deleted_at IS NOT NULL AND n.id IS NOT NULL) OR (u.result_deleted_at IS NULL AND (n.id IS NULL OR n.blob_id IS NOT u.pending_blob_id OR b.size<>u.length OR lower(hex(b.sha256))<>u.actual_sha256))))");
    if (!evidence) return evidence.error();
    r = evidence.value()->step();
    if (!r) return r.error();
    if (!r.value()) return ErrorCode::IntegrityError;
    ok = evidence.value()->integer(0);
    if (!ok || ok.value() != 1) return ErrorCode::IntegrityError;
    return {};
}
}  // namespace anof::infrastructure::sqlite
