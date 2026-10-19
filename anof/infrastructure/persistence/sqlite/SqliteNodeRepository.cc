#include <anof/domain/drive/DirectoryPolicy.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteNodeRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSpaceRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
#include <sqlite3.h>

#include <algorithm>
#include <set>

namespace anof::infrastructure::sqlite {
namespace {

struct StoredNode {
    DirectorySnapshot metadata;
    bool file;
    bool trashed;
};

Result<StoredNode> decodeNode(const SqliteStatement& statement) {
    auto id = readId<NodeId>(statement, 0);
    auto space = readId<SpaceId>(statement, 1);
    auto parentNull = statement.isNull(2);
    auto name = statement.boundedTextView(3, 255);
    auto creator = readId<UserId>(statement, 4);
    auto revision = readQuantity<Revision>(statement, 5);
    auto created = readTime(statement, 6);
    auto updated = readTime(statement, 7);
    auto kind = statement.integer(8);
    auto trashed = readBoolean(statement, 9);
    auto blobNull = statement.isNull(10);
    if (!id || !space || !parentNull || !name || !creator || !revision || !created || !updated || !kind || !trashed || !blobNull) {
        return ErrorCode::IntegrityError;
    }
    if (kind.value() != 0 && kind.value() != 1) return ErrorCode::IntegrityError;
    const bool file = kind.value() == 1;
    if (file == blobNull.value()) return ErrorCode::IntegrityError;
    if (file && !readId<BlobId>(statement, 10)) return ErrorCode::IntegrityError;
    std::optional<NodeId> parent;
    if (!parentNull.value()) {
        auto value = readId<NodeId>(statement, 2);
        if (!value) return value.error();
        parent = value.value();
    }
    if (!name.value().empty() && !validateDirectoryName(name.value())) return ErrorCode::IntegrityError;
    if (name.value().empty() && (parent || file || trashed.value())) return ErrorCode::IntegrityError;
    return StoredNode{ { id.value(), space.value(), parent, std::string(name.value()), creator.value(), revision.value(), created.value(), updated.value() }, file, trashed.value() };
}

constexpr const char* detailsQuery =
    "SELECT n.id,n.space_id,n.parent_id,n.name,n.created_by,n.revision,n.created_at,n.updated_at,n.kind,n.trashed,n.blob_id,b.size,b.sha256 "
    "FROM nodes n LEFT JOIN blobs b ON b.id=n.blob_id ";

Result<NodeDetails> decodeDetails(const SqliteStatement& statement) {
    auto node = decodeNode(statement);
    if (!node) return node.error();
    if (node.value().trashed) return ErrorCode::IntegrityError;
    NodeDetails result{ std::move(node.value().metadata), DirectoryContent{} };
    if (node.value().file) {
        auto size = readQuantity<ByteCount>(statement, 11);
        auto digest = statement.blob(12);
        if (!size || !digest || digest.value().size() != 32) return ErrorCode::IntegrityError;
        FileContentMetadata content{ size.value(), {} };
        std::copy(digest.value().begin(), digest.value().end(), content.sha256.begin());
        result.content = content;
    } else {
        auto size = statement.isNull(11);
        auto digest = statement.isNull(12);
        if (!size || !digest || !size.value() || !digest.value()) return ErrorCode::IntegrityError;
    }
    return result;
}

Result<bool> idExists(SqliteConnection& connection, NodeId id) {
    auto prepared = SqliteStatement::prepare(connection, "SELECT 1 FROM nodes WHERE id=?1");
    if (!prepared) return prepared.error();
    auto bound = prepared.value()->bindText(1, id.toString());
    if (!bound) return bound.error();
    return prepared.value()->step();
}

Result<bool> nameExists(SqliteConnection& connection, const NewDirectory& directory) {
    auto prepared = SqliteStatement::prepare(connection,
                                             "SELECT 1 FROM nodes WHERE space_id=?1 AND parent_id=?2 AND name=?3 COLLATE BINARY AND trashed=0");
    if (!prepared) return prepared.error();
    auto& statement = *prepared.value();
    auto bound = statement.bindText(1, directory.spaceId.toString());
    if (!bound) return bound.error();
    bound = statement.bindText(2, directory.parentId.toString());
    if (!bound) return bound.error();
    bound = statement.bindText(3, directory.name);
    if (!bound) return bound.error();
    return statement.step();
}

}  // namespace

Result<DirectoryPath> SqliteNodeRepository::readPath(const Space& space, NodeId parentId) {
    DirectoryPath reversePath;
    std::set<NodeId> visited;
    NodeId current = parentId;
    bool targetIsFile = false;
    // 一次只读一个祖先，最多 129 个节点；不能依赖损坏树上的无限递归查询。
    for (std::size_t depth = 0; depth <= kMaxDirectoryDepth; ++depth) {
        if (!visited.insert(current).second) return ErrorCode::IntegrityError;
        auto prepared = SqliteStatement::prepare(context_.connection(),
                                                 "SELECT id, space_id, parent_id, name, created_by, revision, created_at, updated_at, kind, trashed, blob_id "
                                                 "FROM nodes WHERE id=?1");
        if (!prepared) return prepared.error();
        auto& statement = *prepared.value();
        auto bound = statement.bindText(1, current.toString());
        if (!bound) return bound.error();
        auto row = statement.step();
        if (!row) return row.error();
        if (!row.value()) return depth == 0 ? ErrorCode::NotFound : ErrorCode::IntegrityError;
        auto decoded = decodeNode(statement);
        if (!decoded) return decoded.error();
        auto& node = decoded.value();
        if (node.metadata.id != current) return ErrorCode::IntegrityError;
        if (node.metadata.spaceId != space.id) return depth == 0 ? ErrorCode::NotFound : ErrorCode::IntegrityError;
        if (node.trashed) return ErrorCode::NotFound;
        if (depth == 0)
            targetIsFile = node.file;
        else if (node.file)
            return ErrorCode::IntegrityError;
        if (current == space.rootNodeId) {
            if (node.file || node.metadata.parentId || !node.metadata.name.empty()) return ErrorCode::IntegrityError;
            // 先验证祖先可见性，再报告目标文件类型，避免泄漏隐藏子树的信息。
            if (targetIsFile) return ErrorCode::ParentNotDirectory;
            reversePath.push_back(std::move(node.metadata));
            std::reverse(reversePath.begin(), reversePath.end());
            return reversePath;
        }
        if (!node.metadata.parentId || node.metadata.name.empty()) return ErrorCode::IntegrityError;
        current = *node.metadata.parentId;
        reversePath.push_back(std::move(node.metadata));
    }
    return ErrorCode::IntegrityError;
}

Result<DirectoryPath> SqliteNodeRepository::findActiveDirectoryPath(SpaceId spaceId, NodeId parentId) {
    auto ready = context_.requireOpen();
    if (!ready) return ready.error();
    SqliteSpaceRepository spaces(context_);
    auto space = spaces.find(spaceId);
    if (!space) return space.error();
    if (!space.value()) return context_.fail(ErrorCode::NotFound);
    auto path = readPath(*space.value(), parentId);
    if (!path) return context_.fail(path.error());
    return std::move(path).value();
}

Result<std::optional<NodeDetails>> SqliteNodeRepository::find(NodeId id) {
    auto query = detail::query(context_, std::string(detailsQuery) + "WHERE n.id=?1 AND n.trashed=0", { id.toString() });
    if (!query) return query.error();
    auto row = query.value()->step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return std::optional<NodeDetails>{};
    auto node = decodeDetails(*query.value());
    if (!node) return context_.fail(node.error());
    SqliteSpaceRepository spaces(context_);
    auto space = spaces.find(node.value().metadata.spaceId);
    if (!space) return space.error();
    if (!space.value()) return context_.fail(ErrorCode::IntegrityError);
    auto path = readPath(*space.value(), id);
    if (!path) {
        if (path.error().code == ErrorCode::NotFound) return std::optional<NodeDetails>{};
        if (path.error().code != ErrorCode::ParentNotDirectory) return context_.fail(path.error());
    }
    return std::optional<NodeDetails>{ std::move(node).value() };
}

Result<Page<NodeDetails, ChildPosition>> SqliteNodeRepository::listChildren(
    SpaceId space, NodeId parent, PageSize size, std::optional<ChildPosition> after) {
    auto path = findActiveDirectoryPath(space, parent);
    if (!path) return path.error();
    if (after && !validateDirectoryName(after->name)) return context_.fail(ErrorCode::InvalidArgument);
    auto query = after ? detail::query(context_, std::string(detailsQuery) + "WHERE n.space_id=?1 AND n.parent_id=?2 AND n.trashed=0 AND (n.name COLLATE BINARY,n.id)>(?3,?4) ORDER BY n.name COLLATE BINARY,n.id LIMIT ?5",
                                       { space.toString(), parent.toString(), after->name, after->id.toString(), size.value() + 1 })
                       : detail::query(context_, std::string(detailsQuery) + "WHERE n.space_id=?1 AND n.parent_id=?2 AND n.trashed=0 ORDER BY n.name COLLATE BINARY,n.id LIMIT ?3",
                                       { space.toString(), parent.toString(), size.value() + 1 });
    if (!query) return query.error();
    Page<NodeDetails, ChildPosition> result;
    for (;;) {
        auto row = query.value()->step();
        if (!row) return context_.fail(row.error());
        if (!row.value()) return result;
        auto node = decodeDetails(*query.value());
        if (!node) return context_.fail(node.error());
        if (result.items.size() == static_cast<std::size_t>(size.value())) {
            const auto& last = result.items.back().metadata;
            result.next = ChildPosition{ last.name, last.id };
            return result;
        }
        result.items.push_back(std::move(node).value());
    }
}

Result<void> SqliteNodeRepository::insertRoot(const RootDirectory& root) {
    SqliteSpaceRepository spaces(context_);
    auto space = spaces.find(root.spaceId);
    if (!space) return space.error();
    if (!space.value() || space.value()->rootNodeId != root.id) return context_.fail(ErrorCode::IntegrityError);
    if (space.value()->ownerId && *space.value()->ownerId != root.createdBy) return context_.fail(ErrorCode::IntegrityError);
    if (space.value()->kind == SpaceKind::Public) {
        auto creator = detail::query(context_, "SELECT 1 FROM users WHERE id=?1 AND role=1 AND enabled=1", { root.createdBy.toString() });
        if (!creator) return creator.error();
        auto row = creator.value()->step();
        if (!row) return context_.fail(row.error());
        if (!row.value()) return context_.fail(ErrorCode::IntegrityError);
    }
    return detail::one(context_, detail::write(context_,
                                               "INSERT INTO nodes(id,space_id,parent_id,name,kind,created_by,blob_id,revision,trashed,created_at,updated_at) VALUES(?1,?2,NULL,'',0,?3,NULL,1,0,?4,?4)",
                                               { root.id.toString(), root.spaceId.toString(), root.createdBy.toString(), root.createdAt.time_since_epoch().count() }));
}

Result<DirectorySnapshot> SqliteNodeRepository::insertDirectory(const NewDirectory& directory) {
    auto ready = context_.requireOpen();
    if (!ready) return ready.error();
    auto valid = validateDirectoryName(directory.name);
    if (!valid) return context_.fail(valid.error());
    SqliteSpaceRepository spaces(context_);
    auto space = spaces.find(directory.spaceId);
    if (!space) return space.error();
    if (!space.value()) return context_.fail(ErrorCode::NotFound);
    auto path = readPath(*space.value(), directory.parentId);
    if (!path) return context_.fail(path.error());
    valid = validateDirectoryParent(*space.value(), directory.parentId, path.value());
    if (!valid) return context_.fail(valid.error());
    auto existing = idExists(context_.connection(), directory.id);
    if (!existing) return context_.fail(existing.error());
    if (existing.value()) return context_.fail(ErrorCode::IntegrityError);
    auto prepared = SqliteStatement::prepare(context_.connection(),
                                             "INSERT INTO nodes(id, space_id, parent_id, name, kind, created_by, blob_id, revision, trashed, created_at, updated_at) "
                                             "VALUES (?1, ?2, ?3, ?4, 0, ?5, NULL, 1, 0, ?6, ?6)");
    if (!prepared) return context_.fail(prepared.error());
    auto& statement = *prepared.value();
    auto bound = statement.bindText(1, directory.id.toString());
    if (!bound) return context_.fail(bound.error());
    bound = statement.bindText(2, directory.spaceId.toString());
    if (!bound) return context_.fail(bound.error());
    bound = statement.bindText(3, directory.parentId.toString());
    if (!bound) return context_.fail(bound.error());
    bound = statement.bindText(4, directory.name);
    if (!bound) return context_.fail(bound.error());
    bound = statement.bindText(5, directory.createdBy.toString());
    if (!bound) return context_.fail(bound.error());
    bound = statement.bindInt64(6, directory.createdAt.time_since_epoch().count());
    if (!bound) return context_.fail(bound.error());
    auto row = statement.step();
    if (!row) {
        // 只将已确认的活动名称冲突映射为 NameConflict；不解析 SQLite 错误字符串。
        const int code = context_.connection().diagnostic().extendedCode;
        if (code == SQLITE_CONSTRAINT_UNIQUE) {
            auto duplicate = nameExists(context_.connection(), directory);
            if (!duplicate) return context_.fail(duplicate.error());
            if (duplicate.value()) return context_.fail(ErrorCode::NameConflict);
        }
        return context_.fail(row.error());
    }
    if (row.value() || sqlite3_changes(context_.connection().handle()) != 1) return context_.fail(ErrorCode::IntegrityError);
    return DirectorySnapshot{ directory.id, directory.spaceId, directory.parentId, directory.name, directory.createdBy,
                              Revision{}, directory.createdAt, directory.createdAt };
}

Result<Revision> SqliteNodeRepository::advanceRevision(NodeId id, Revision expected, UtcTime now) {
    auto ready = context_.requireOpen();
    if (!ready) return ready.error();
    auto prepared = SqliteStatement::prepare(context_.connection(), "SELECT revision,created_at,updated_at FROM nodes WHERE id=?1");
    if (!prepared) return context_.fail(prepared.error());
    auto bound = prepared.value()->bindText(1, id.toString());
    if (!bound) return context_.fail(bound.error());
    auto row = prepared.value()->step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return context_.fail(ErrorCode::NotFound);
    auto current = readQuantity<Revision>(*prepared.value(), 0);
    if (!current) return context_.fail(current.error());
    if (current.value() != expected) return context_.fail(ErrorCode::Conflict);
    auto next = current.value().add(1);
    if (!next) return context_.fail(next.error());
    auto created = readTime(*prepared.value(), 1);
    auto updated = readTime(*prepared.value(), 2);
    if (!created || !updated) return context_.fail(ErrorCode::IntegrityError);
    now = nondecreasingEventTime(now, created.value(), updated.value());
    prepared.value().reset();
    auto update = SqliteStatement::prepare(context_.connection(),
                                           "UPDATE nodes SET revision=?1, updated_at=?2 WHERE id=?3 AND revision=?4");
    if (!update) return context_.fail(update.error());
    bound = update.value()->bindInt64(1, next.value().value());
    if (!bound) return context_.fail(bound.error());
    bound = update.value()->bindInt64(2, now.time_since_epoch().count());
    if (!bound) return context_.fail(bound.error());
    bound = update.value()->bindText(3, id.toString());
    if (!bound) return context_.fail(bound.error());
    bound = update.value()->bindInt64(4, expected.value());
    if (!bound) return context_.fail(bound.error());
    row = update.value()->step();
    if (!row) return context_.fail(row.error());
    if (row.value()) return context_.fail(ErrorCode::IntegrityError);
    if (sqlite3_changes(context_.connection().handle()) != 1) return context_.fail(ErrorCode::Conflict);
    return next.value();
}

Result<NodeDetails> SqliteNodeRepository::insertFile(const NewFile& file) {
    const auto& directory = file.metadata;
    auto ready = context_.requireOpen();
    if (!ready) return ready.error();
    auto valid = validateDirectoryName(directory.name);
    if (!valid) return context_.fail(valid.error());
    SqliteSpaceRepository spaces(context_);
    auto space = spaces.find(directory.spaceId);
    if (!space) return space.error();
    if (!space.value()) return context_.fail(ErrorCode::NotFound);
    auto path = readPath(*space.value(), directory.parentId);
    if (!path) return context_.fail(path.error());
    valid = validateDirectoryParent(*space.value(), directory.parentId, path.value());
    if (!valid) return context_.fail(valid.error());
    auto existing = idExists(context_.connection(), directory.id);
    if (!existing) return context_.fail(existing.error());
    if (existing.value()) return context_.fail(ErrorCode::IntegrityError);
    auto duplicate = nameExists(context_.connection(), directory);
    if (!duplicate) return context_.fail(duplicate.error());
    if (duplicate.value()) return context_.fail(ErrorCode::NameConflict);
    auto written = detail::one(context_, detail::write(context_,
                                                       "INSERT INTO nodes(id,space_id,parent_id,name,kind,created_by,blob_id,revision,trashed,created_at,updated_at) VALUES(?1,?2,?3,?4,1,?5,?6,1,0,?7,?7)",
                                                       { directory.id.toString(), directory.spaceId.toString(), directory.parentId.toString(), directory.name, directory.createdBy.toString(), file.blobId.toString(), directory.createdAt.time_since_epoch().count() }));
    if (!written) return written.error();
    auto value = find(directory.id);
    if (!value) return value.error();
    if (!value.value()) return context_.fail(ErrorCode::IntegrityError);
    return *value.value();
}
}  // namespace anof::infrastructure::sqlite
