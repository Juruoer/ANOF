#pragma once

#include <anof/application/ports/MetadataTransaction.h>
#include <anof/infrastructure/persistence/sqlite/SqliteAuthorizationRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSpaceRepository.h>

#include <anof/infrastructure/persistence/sqlite/SqliteNodeRepository.h>

#include <anof/infrastructure/persistence/sqlite/SqliteQuotaRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteAuditRepository.h>

namespace anof::infrastructure::sqlite {

// 内部事务实现；生产代码由 MetadataStore 创建，构造本身不启动 SQL 事务。
class SqliteMetadataTransaction final : public MetadataTransaction {
public:
    SqliteMetadataTransaction(SqliteConnection& connection, bool& storeActive);
    ~SqliteMetadataTransaction() override;
    Result<void> begin();
    AuthorizationRepository& authorization() override { return authorization_; }
    SpaceRepository& spaces() override { return spaces_; }
    NodeRepository& nodes() override { return nodes_; }
    QuotaRepository& quotas() override { return quotas_; }
    AuditRepository& audit() override { return audit_; }
    Result<void> commit() override { return context_.commit(); }

private:
    SqliteTransactionContext context_;
    SqliteAuthorizationRepository authorization_;
    SqliteSpaceRepository spaces_;
    SqliteNodeRepository nodes_;
    SqliteQuotaRepository quotas_;
    SqliteAuditRepository audit_;
};

}  // namespace anof::infrastructure::sqlite
