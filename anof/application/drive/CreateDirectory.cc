#include <anof/application/drive/CreateDirectory.h>
#include <anof/domain/drive/DirectoryPolicy.h>

namespace anof {

Result<CreateDirectoryResult> CreateDirectory::execute(const RequestContext& context, const CreateDirectoryCommand& command,
                                                       std::optional<CreateDirectoryAttempt>* attempt) {
    if (attempt) attempt->reset();
    auto validName = validateDirectoryName(command.name);
    if (!validName) return validName.error();
    auto nodeId = nodeIds_.next();
    if (!nodeId) return nodeId.error();
    if (attempt) *attempt = CreateDirectoryAttempt{ nodeId.value(), context.requestId };
    auto opened = metadata_.beginWrite();
    if (!opened) return opened.error();
    auto transaction = std::move(opened).value();
    if (!transaction) return ErrorCode::InternalError;

    // 所有提前返回都由同一事务的 RAII 析构回滚；这里不自动重试任何一步。
    const auto createdAt = clock_.now();
    auto principal = transaction->authorization().requireActiveSession(context.credential, createdAt);
    if (!principal) return principal.error();
    auto found = transaction->spaces().find(command.spaceId);
    if (!found) return found.error();
    if (!found.value()) return ErrorCode::NotFound;
    const auto& space = *found.value();
    auto scopes = directoryQuotaScopes(principal.value(), space);
    if (!scopes) return scopes.error();
    auto path = transaction->nodes().findActiveDirectoryPath(space.id, command.parentId);
    if (!path) return path.error();
    auto parent = validateDirectoryParent(space, command.parentId, path.value());
    if (!parent) return parent.error();

    const QuotaDelta charge{ ByteCount{}, NodeCount::from(1).value() };
    for (const auto& scope : scopes.value()) {
        auto charged = transaction->quotas().chargeUsed(scope, charge);
        if (!charged) return charged.error();
    }
    auto counted = transaction->nodeAccounting().chargeUsed(charge.nodes);
    if (!counted) return counted.error();
    auto directory = transaction->nodes().insertDirectory({ nodeId.value(), space.id, command.parentId,
                                                           command.name, principal.value().userId, createdAt });
    if (!directory) return directory.error();
    auto revision = transaction->nodes().advanceRevision(command.parentId, path.value().back().revision, createdAt);
    if (!revision) return revision.error();

    // 复验自然到期及身份一致性；账户／权限的并发修改仍由事务隔离保护。
    const auto checkedAt = clock_.now();
    auto verified = transaction->authorization().requireActiveSession(context.credential, checkedAt);
    if (!verified) return verified.error();
    if (verified.value().userId != principal.value().userId || verified.value().authEpoch != principal.value().authEpoch ||
        verified.value().role != principal.value().role) {
        return ErrorCode::Unauthenticated;
    }
    auto audited = transaction->directoryAudit().append({ principal.value().userId, space.id, nodeId.value(),
                                                command.parentId, context.requestId, checkedAt });
    if (!audited) return audited.error();

    // 在 commit 前组装返回值，避免提交成功后再分配目录名称存储。
    CreateDirectoryResult result{ std::move(directory).value(), revision.value() };
    auto committed = transaction->commit();
    if (!committed) return committed.error();
    return result;
}

}  // namespace anof
