#pragma once
#include <anof/application/ports/TransferMetadataStore.h>

#include <atomic>
namespace anof::test {
struct CommitFault {
    std::atomic<bool> armed{ false };
    bool afterCommit = true;
    std::atomic<unsigned> calls{ 0 };
};
class TransferFaultStore final : public TransferMetadataStore {
    struct Transaction final : TransferTransaction {
        std::unique_ptr<TransferTransaction> tx;
        CommitFault& fault;
        Transaction(std::unique_ptr<TransferTransaction> t, CommitFault& f) : tx(std::move(t)), fault(f) {}
        AuthorizationReader& authorization() override { return tx->authorization(); }
        UserRepository& users() override { return tx->users(); }
        SessionRepository& sessions() override { return tx->sessions(); }
        InviteRepository& invites() override { return tx->invites(); }
        SpaceRepository& spaces() override { return tx->spaces(); }
        FileNodeRepository& nodes() override { return tx->nodes(); }
        ReservationQuotaRepository& quotas() override { return tx->quotas(); }
        SettingsRepository& settings() override { return tx->settings(); }
        AuditRepository& directoryAudit() override { return tx->directoryAudit(); }
        IdentityAuditRepository& identityAudit() override { return tx->identityAudit(); }
        UploadRepository& uploads() override { return tx->uploads(); }
        BlobRepository& blobs() override { return tx->blobs(); }
        StorageRepository& storage() override { return tx->storage(); }
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
    Result<std::unique_ptr<TransferTransaction>> beginTransferWrite() override {
        auto tx = store_->beginTransferWrite();
        if (!tx) return tx.error();
        return std::unique_ptr<TransferTransaction>(new Transaction(std::move(tx).value(), fault_));
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
