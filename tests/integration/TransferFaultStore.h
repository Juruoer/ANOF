#pragma once
#include <anof/application/ports/DriveMetadataStore.h>

#include <atomic>
namespace anof::test {
struct CommitFault {
    std::atomic<bool> armed{ false };
    bool afterCommit = true;
    std::atomic<unsigned> calls{ 0 };
};
class TransferFaultStore final : public DriveMetadataStore {
    struct Transaction final : DriveTransaction {
        std::unique_ptr<DriveTransaction> tx;
        CommitFault& fault;
        Transaction(std::unique_ptr<DriveTransaction> t, CommitFault& f) : tx(std::move(t)), fault(f) {}
        TreeRepository& tree() override { return tx->tree(); }
        TrashRepository& trash() override { return tx->trash(); }
        DriveAuditRepository& driveAudit() override { return tx->driveAudit(); }
        NodeAccountingRepository& nodeAccounting() override { return tx->nodeAccounting(); }
        AuthorizationReader& authorization() override { return tx->authorization(); }
        UserRepository& users() override { return tx->users(); }
        SessionRepository& sessions() override { return tx->sessions(); }
        InviteRepository& invites() override { return tx->invites(); }
        SpaceRepository& spaces() override { return tx->spaces(); }
        FileNodeRepository& nodes() override { return tx->nodes(); }
        DriveQuotaRepository& quotas() override { return tx->quotas(); }
        SettingsRepository& settings() override { return tx->settings(); }
        AuditRepository& directoryAudit() override { return tx->directoryAudit(); }
        IdentityAuditRepository& identityAudit() override { return tx->identityAudit(); }
        DriveUploadRepository& uploads() override { return tx->uploads(); }
        LifecycleBlobRepository& blobs() override { return tx->blobs(); }
        DriveStorageRepository& storage() override { return tx->storage(); }
        TransferAuditRepository& transferAudit() override { return tx->transferAudit(); }
        Result<void> commit() override {
            if (!fault.armed.exchange(false)) return tx->commit();
            ++fault.calls;
            if (fault.afterCommit) {
                auto result = tx->commit();
                if (!result) return result;
            }
            return ErrorCode::CommitOutcomeUnknown;
        }
    };
    std::unique_ptr<TransferMetadataStore> store_;
    CommitFault& fault_;

public:
    TransferFaultStore(std::unique_ptr<TransferMetadataStore> store, CommitFault& fault)
        : store_(std::move(store)), fault_(fault) {}
    Result<std::unique_ptr<TransferReadSnapshot>> beginTransferRead() override { return store_->beginTransferRead(); }
    Result<std::unique_ptr<DriveReadSnapshot>> beginDriveRead() override {
        auto* drive = dynamic_cast<DriveMetadataStore*>(store_.get());
        if (!drive) return ErrorCode::InternalError;
        return drive->beginDriveRead();
    }
    Result<std::unique_ptr<DriveTransaction>> beginDriveWrite() override {
        auto* drive = dynamic_cast<DriveMetadataStore*>(store_.get());
        if (!drive) return ErrorCode::InternalError;
        auto tx = drive->beginDriveWrite();
        if (!tx) return tx.error();
        return std::unique_ptr<DriveTransaction>(new Transaction(std::move(tx).value(), fault_));
    }
    Result<std::unique_ptr<TransferTransaction>> beginTransferWrite() override {
        auto tx = beginDriveWrite();
        if (!tx) return tx.error();
        return std::unique_ptr<TransferTransaction>(std::move(tx).value());
    }
    Result<std::unique_ptr<MetadataReadSnapshot>> beginRead() override {
        auto read = beginTransferRead();
        if (!read) return read.error();
        return std::unique_ptr<MetadataReadSnapshot>(std::move(read).value());
    }
    Result<std::unique_ptr<MetadataTransaction>> beginWrite() override {
        auto tx = beginTransferWrite();
        if (!tx) return tx.error();
        return std::unique_ptr<MetadataTransaction>(std::move(tx).value());
    }
};
}  // namespace anof::test
