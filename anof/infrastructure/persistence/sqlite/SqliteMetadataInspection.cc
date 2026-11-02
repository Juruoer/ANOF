#include <sqlite3.h>
#include <anof/infrastructure/persistence/sqlite/internal/RepairInvariant.h>
#include <anof/infrastructure/persistence/sqlite/SqliteBlobRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataInspection.h>
#include <anof/infrastructure/persistence/sqlite/SqliteNodeAccountingRepository.h>
#include <anof/infrastructure/persistence/sqlite/internal/MaintenanceCall.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStorageRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteUploadRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
#include <anof/infrastructure/persistence/sqlite/internal/MaintenanceSql.h>
#include <anof/infrastructure/system/internal/BoundedFileRead.h>
#include <anof/infrastructure/system/internal/InitializationFiles.h>
#include <anof/infrastructure/system/internal/InstanceFiles.h>
namespace anof::infrastructure::sqlite {
namespace {
using namespace maintenance;
class Reader final : public MetadataInspectionReader {
    OperationId operation_;
    bool invalidated_ = false;
    system::detail::OwnedFd held_;
    std::unique_ptr<SqliteConnection> db_;
    bool active_ = false;
    std::unique_ptr<SqliteTransactionContext> ctx_;
    std::vector<MaintenanceFinding> findings_;
    MaintenanceReport report_;
    std::optional<InstanceId> formatIdentity_;
    InspectionIdentity identity_;
    void finding(FindingCode code, MaintenanceResource resource = MaintenanceResource::Database) {
        report_.errors = report_.errors.add(1).value();
        if (findings_.size() < kMaintenanceFindingLimit)
            findings_.push_back({ code, FindingSeverity::Error, resource, {}, {}, false });
        else {
            report_.complete = false;
            report_.detailsTruncated = true;
        }
    }
    MaintenanceFailure bad(const MaintenanceContext& call, const CallProgress& progress) {
        if (progress.interrupted()) {
            invalidated_ = true;
            report_.complete = false;
            return progress.error(call, ErrorCode::IntegrityError, MaintenanceStage::Inspect);
        }
        finding(FindingCode::DatabaseCorrupt);
        report_.complete = false;
        return failure(call, ErrorCode::IntegrityError, MaintenanceStage::Inspect);
    }

    void inspectIdentity(const MaintenanceContext& call, const CallProgress& progress) {
        if (formatIdentity_) identity_.formatVersion = 5;
        auto q = detail::query(*ctx_, "SELECT instance_id,(SELECT version FROM schema_info WHERE singleton=1) FROM instance_settings WHERE singleton=1");
        if (!q) {
            (void)bad(call, progress);
            return;
        }
        auto row = q.value()->step();
        if (!row || !row.value()) {
            (void)bad(call, progress);
            return;
        }
        auto id = readId<InstanceId>(*q.value(), 0);
        auto version = q.value()->integer(1);
        if (id) identity_.instance = id.value();
        if (version) identity_.schemaVersion = version.value();
        const bool valid = id && version && version.value() == kSqliteSchemaVersion && formatIdentity_ && *formatIdentity_ == id.value();
        row = q.value()->step();
        if (!valid || !row || row.value()) (void)bad(call, progress);
    }

    // 检查副本是独占私有文件；逐调用只读事务，不跨后续文件检查保留SQL事务。
    struct ReadCall {
        Reader& reader;
        bool ready=false;
        explicit ReadCall(Reader& r):reader(r) {
            if(!reader.db_)return;
            reader.ctx_=std::make_unique<SqliteTransactionContext>(*reader.db_,reader.active_);
            ready=bool(reader.ctx_->beginRead());
        }
        ~ReadCall(){reader.ctx_.reset();}
    };
public:
    Reader(const MaintenanceContext& call, system::detail::OwnedFd held, std::unique_ptr<SqliteConnection> db, std::optional<InstanceId> format) : operation_(call.operationId), held_(std::move(held)), db_(std::move(db)), formatIdentity_(format) {}
    MaintenanceResult<void> initialize(const MaintenanceContext& call) {
        CallProgress progress(db_.get(), call);
        inspect(call, progress);
        if (db_) sqlite3_progress_handler(db_->handle(), 0, nullptr, nullptr);
        ctx_.reset();
        if (progress.interrupted() || call.cancelled.load()) {
            auto out = progress.error(call, ErrorCode::IntegrityError, MaintenanceStage::Inspect);
            if (call.cancelled.load()) out.code = MaintenanceError::Cancelled;
            return out;
        }
        return {};
    }
    void inspect(const MaintenanceContext& call, const CallProgress& progress) {
        if (!formatIdentity_) {
            finding(FindingCode::DatabaseCorrupt);
            report_.complete = false;
        }
        if (!db_) {
            finding(FindingCode::DatabaseCorrupt);
            report_.complete = false;
            return;
        }
        // SQLite 的日志恢复只可能写入 capture。业务查询随后固定为 query_only。
        if (!db_->execute("PRAGMA query_only=ON")) {
            finding(FindingCode::DatabaseCorrupt);
            report_.complete = false;
            return;
        }
        const bool schemaValid = bool(verifySqliteSchema(*db_));
        ctx_ = std::make_unique<SqliteTransactionContext>(*db_, active_);
        if (!ctx_->beginRead()) {
            finding(FindingCode::DatabaseCorrupt);
            report_.complete = false;
            return;
        }
        inspectIdentity(call, progress);
        for (auto sql : { "PRAGMA integrity_check(10001)", "PRAGMA foreign_key_check" }) {
            auto q = SqliteStatement::prepare(*db_, sql);
            if (!q) {
                finding(FindingCode::DatabaseCorrupt);
                report_.complete = false;
                continue;
            }
            std::size_t count = 0;
            for (;;) {
                auto row = q.value()->step();
                if (!row) {
                    finding(FindingCode::DatabaseCorrupt);
                    report_.complete = false;
                    break;
                }
                if (!row.value()) break;
                if (++count > kMaintenanceFindingLimit) {
                    finding(FindingCode::CheckLimitExceeded);
                    report_.complete = false;
                    break;
                }
                if (sql[7] == 'i') {
                    auto text = q.value()->boundedTextView(0, 4096);
                    if (!text || text.value() != "ok") finding(FindingCode::DatabaseCorrupt);
                } else
                    finding(FindingCode::ForeignKeyBroken);
            }
        }
        if (!schemaValid) {
            finding(FindingCode::DatabaseCorrupt);
            report_.complete = false;
            return;
        }
        for (const char* table : { "blobs", "uploads", "maintenance_runs", "maintenance_actions" }) {
            auto rows = detail::query(*ctx_, std::string("SELECT id") + (std::string(table) == "maintenance_actions" ? ",run_id" : "") + " FROM " + table + " ORDER BY id");
            if (!rows) {
                finding(FindingCode::DatabaseCorrupt);
                report_.complete = false;
                return;
            }
            std::size_t count = 0;
            for (;;) {
                auto row = rows.value()->step();
                if (!row) {
                    finding(progress.interrupted() ? FindingCode::CheckLimitExceeded : FindingCode::DatabaseCorrupt);
                    report_.complete = false;
                    return;
                }
                if (!row.value()) break;
                if (++count > 100000) {
                    finding(FindingCode::CheckLimitExceeded);
                    report_.complete = false;
                    return;
                }
                bool valid = false;
                auto text = rows.value()->text(0);
                if (text) {
                    if (std::string(table) == "blobs") {
                        auto id = BlobId::parse(text.value());
                        if (id) {
                            SqliteBlobRepository repo(*ctx_);
                            auto b = repo.record(id.value());
                            valid = b && bool(b.value());
                        }
                    } else if (std::string(table) == "uploads") {
                        auto id = UploadId::parse(text.value());
                        if (id) {
                            SqliteUploadRepository repo(*ctx_);
                            auto u = repo.find(id.value());
                            valid = u && bool(u.value());
                        }
                    } else if (std::string(table) == "maintenance_runs") {
                        auto id = MaintenanceRunId::parse(text.value());
                        if (id) {
                            auto r = readRun(*ctx_, id.value());
                            valid = r && bool(r.value());
                        }
                    } else {
                        auto id = MaintenanceActionId::parse(text.value());
                        auto run = readId<MaintenanceRunId>(*rows.value(), 1);
                        if (id && run) {
                            auto a = readAction(*ctx_, run.value(), id.value());
                            valid = a && bool(a.value());
                        }
                    }
                }
                if (!valid) {
                    finding(FindingCode::DatabaseCorrupt);
                    report_.complete = false;
                    return;
                }
            }
        }
        auto ledger = accountingDigest(*ctx_, false);
        if (!ledger) {
            finding(FindingCode::TreeInvalid);
            report_.complete = false;
            return;
        }
        if (!verifyRecordedAccounting(*ctx_)) finding(FindingCode::AccountingMismatch);
        auto pending = detail::query(*ctx_, "SELECT EXISTS(SELECT 1 FROM quarantine_entries),EXISTS(SELECT 1 FROM maintenance_runs WHERE completed=0)");
        if (!pending) {
            report_.complete = false;
            return;
        }
        auto r = pending.value()->step();
        if (!r || !r.value()) {
            report_.complete = false;
            return;
        }
        if (pending.value()->integer(0).value()) finding(FindingCode::QuarantinePresent, MaintenanceResource::Quarantine);
        if (pending.value()->integer(1).value()) finding(FindingCode::MaintenancePending, MaintenanceResource::Run);
    }
    MaintenanceResult<InspectionIdentity> identity(const MaintenanceContext& call) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Inspect);
        if (!valid) return valid.error();
        if (invalidated_) return failure(call, ErrorCode::InvalidArgument, MaintenanceStage::Inspect);
        CallProgress progress(db_.get(), call);
        return identity_;
    }
    MaintenanceResult<Page<MaintenanceFinding, InspectionPosition>> findings(const MaintenanceContext& call, PageSize size, std::optional<InspectionPosition> after) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Inspect);
        if (!valid) return valid.error();
        if (invalidated_) return failure(call, ErrorCode::InvalidArgument, MaintenanceStage::Inspect);
        CallProgress progress(db_.get(), call);

        Page<MaintenanceFinding, InspectionPosition> page;
        auto start = after ? static_cast<std::uint64_t>(after->value()) + 1 : 0;
        for (auto i = start; i < findings_.size(); ++i) {
            if (page.items.size() == static_cast<std::size_t>(size.value())) {
                page.next = InspectionPosition::from(static_cast<std::int64_t>(i - 1)).value();
                break;
            }
            page.items.push_back(findings_[i]);
        }
        return page;
    }
    MaintenanceResult<Page<InspectedObject, BlobId>> objects(const MaintenanceContext& call, PageSize size, std::optional<BlobId> after) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Inspect);
        if (!valid) return valid.error();
        if (invalidated_) return failure(call, ErrorCode::InvalidArgument, MaintenanceStage::Inspect);
        ReadCall read(*this);
        CallProgress progress(db_.get(), call);

        if (!read.ready) return bad(call, progress);
        auto q = detail::query(*ctx_, "SELECT id FROM blobs WHERE (?1 IS NULL OR id>?1) ORDER BY id LIMIT ?2", { after ? detail::Parameter(after->toString()) : detail::Parameter(nullptr), size.value() + 1 });
        if (!q) return bad(call, progress);
        Page<InspectedObject, BlobId> page;
        SqliteBlobRepository repo(*ctx_);
        for (;;) {
            auto r = q.value()->step();
            if (!r) return bad(call, progress);
            if (!r.value()) return page;
            if (page.items.size() == static_cast<std::size_t>(size.value())) {
                page.next = page.items.back().record.content.id;
                return page;
            }
            auto id = readId<BlobId>(*q.value(), 0);
            if (!id) {
                if (!progress.interrupted()) finding(FindingCode::DatabaseCorrupt, MaintenanceResource::Blob);
                return bad(call, progress);
            }
            auto b = repo.record(id.value());
            auto n = repo.hasNodeReference(id.value()), u = repo.hasUploadReference(id.value());
            if (!b || !b.value() || !n || !u) {
                if (!progress.interrupted()) finding(FindingCode::DatabaseCorrupt, MaintenanceResource::Blob);
                return bad(call, progress);
            }
            page.items.push_back({ *b.value(), n.value(), u.value() });
        }
    }
    MaintenanceResult<Page<Upload, UploadId>> uploads(const MaintenanceContext& call, PageSize size, std::optional<UploadId> after) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Inspect);
        if (!valid) return valid.error();
        if (invalidated_) return failure(call, ErrorCode::InvalidArgument, MaintenanceStage::Inspect);
        ReadCall read(*this);
        CallProgress progress(db_.get(), call);

        if (!read.ready) return bad(call, progress);
        auto q = detail::query(*ctx_, "SELECT id FROM uploads WHERE (?1 IS NULL OR id>?1) ORDER BY id LIMIT ?2", { after ? detail::Parameter(after->toString()) : detail::Parameter(nullptr), size.value() + 1 });
        if (!q) return bad(call, progress);
        Page<Upload, UploadId> page;
        SqliteUploadRepository repo(*ctx_);
        for (;;) {
            auto r = q.value()->step();
            if (!r) return bad(call, progress);
            if (!r.value()) return page;
            if (page.items.size() == static_cast<std::size_t>(size.value())) {
                page.next = page.items.back().id;
                return page;
            }
            auto id = readId<UploadId>(*q.value(), 0);
            if (!id) return bad(call, progress);
            auto u = repo.find(id.value());
            if (!u || !u.value()) {
                if (!progress.interrupted()) finding(FindingCode::UploadStateInconsistent, MaintenanceResource::Upload);
                return bad(call, progress);
            }
            page.items.push_back(*u.value());
        }
    }
    MaintenanceResult<Page<MaintenanceRun, MaintenanceRunId>> runs(const MaintenanceContext& call, PageSize size, std::optional<MaintenanceRunId> after) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Inspect);
        if (!valid) return valid.error();
        if (invalidated_) return failure(call, ErrorCode::InvalidArgument, MaintenanceStage::Inspect);
        ReadCall read(*this);
        CallProgress progress(db_.get(), call);

        if (!read.ready) return bad(call, progress);
        auto q = detail::query(*ctx_, "SELECT id FROM maintenance_runs WHERE (?1 IS NULL OR id>?1) ORDER BY id LIMIT ?2", { after ? detail::Parameter(after->toString()) : detail::Parameter(nullptr), size.value() + 1 });
        if (!q) return bad(call, progress);
        Page<MaintenanceRun, MaintenanceRunId> page;
        for (;;) {
            auto r = q.value()->step();
            if (!r) return bad(call, progress);
            if (!r.value()) return page;
            if (page.items.size() == static_cast<std::size_t>(size.value())) {
                page.next = page.items.back().id;
                return page;
            }
            auto id = readId<MaintenanceRunId>(*q.value(), 0);
            if (!id) return bad(call, progress);
            auto run = readRun(*ctx_, id.value());
            if (!run || !run.value()) return bad(call, progress);
            page.items.push_back(*run.value());
        }
    }
    MaintenanceResult<Page<QuarantineEntry, QuarantineId>> quarantine(const MaintenanceContext& call, PageSize size, std::optional<QuarantineId> after) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Inspect);
        if (!valid) return valid.error();
        if (invalidated_) return failure(call, ErrorCode::InvalidArgument, MaintenanceStage::Inspect);
        ReadCall read(*this);
        CallProgress progress(db_.get(), call);

        if (!read.ready) return bad(call, progress);
        auto q = detail::query(*ctx_, "SELECT id,source_area,source_id,evidence,sha256,charged_bytes,state,plan_id,action_id,created_at,updated_at,revision FROM quarantine_entries WHERE (?1 IS NULL OR id>?1) ORDER BY id LIMIT ?2", { after ? detail::Parameter(after->toString()) : detail::Parameter(nullptr), size.value() + 1 });
        if (!q) return bad(call, progress);
        Page<QuarantineEntry, QuarantineId> page;
        for (;;) {
            auto r = q.value()->step();
            if (!r) return bad(call, progress);
            if (!r.value()) return page;
            if (page.items.size() == static_cast<std::size_t>(size.value())) {
                page.next = page.items.back().id;
                return page;
            }
            try {
                auto& s = *q.value();
                Json j = Json::array({ s.text(0).value(), Json::array({ s.integer(1).value(), s.text(2).value() }), Json::parse(s.boundedTextView(3, 4096).value()), s.text(4).value(), s.integer(5).value(), s.integer(6).value(), s.text(7).value(), s.text(8).value(), s.integer(9).value(), s.integer(10).value(), s.integer(11).value() });
                auto e = maintenance::quarantine(j);
                if (!validateQuarantineEntry(e)) return bad(call, progress);
                page.items.push_back(e);
            } catch (const std::exception&) {
                return bad(call, progress);
            }
        }
    }
    MaintenanceResult<AccountingFacts> accounting(const MaintenanceContext& call) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Inspect);
        if (!valid) return valid.error();
        if (invalidated_) return failure(call, ErrorCode::InvalidArgument, MaintenanceStage::Inspect);
        ReadCall read(*this);
        CallProgress progress(db_.get(), call);

        if (!read.ready) return bad(call, progress);
        SqliteStorageRepository storage(*ctx_);
        SqliteNodeAccountingRepository nodes(*ctx_);
        auto s = storage.get();
        auto n = nodes.get();
        auto h = accountingDigest(*ctx_, false);
        if (!s || !n || !h) return bad(call, progress);
        auto q = detail::query(*ctx_, "SELECT coalesce((SELECT sum(charged_bytes) FROM blobs),0),coalesce((SELECT sum(charged_bytes) FROM quarantine_entries),0),coalesce((SELECT sum(length) FROM uploads WHERE state<5 OR cleanup_pending=1),0),(SELECT count(*) FROM nodes),(SELECT count(*) FROM uploads WHERE state<5)");
        if (!q) return bad(call, progress);
        auto row = q.value()->step();
        if (!row || !row.value()) return bad(call, progress);
        auto& v = *q.value();
        auto b = readQuantity<ByteCount>(v, 0), extra = readQuantity<ByteCount>(v, 1), reserved = readQuantity<ByteCount>(v, 2);
        auto used = readQuantity<NodeCount>(v, 3), nr = readQuantity<NodeCount>(v, 4);
        if (!b || !extra || !reserved || !used || !nr) return bad(call, progress);
        auto total = b.value().add(extra.value().value());
        if (!total) return bad(call, progress);
        return AccountingFacts{ s.value(), total.value(), reserved.value(), n.value(), { used.value(), nr.value(), n.value().revision }, h.value(), report_.complete };
    }
    MaintenanceResult<Sha256> repairInvariant(const MaintenanceContext& call, const MaintenancePlan& plan) override {
        auto valid=validateMaintenanceCall(call,operation_,MaintenanceStage::Inspect);if(!valid)return valid.error();
        if(invalidated_)return failure(call,ErrorCode::IntegrityError,MaintenanceStage::Inspect);
        ReadCall read(*this);
        CallProgress progress(db_.get(),call);
        if(!read.ready)return bad(call,progress);
        auto result=maintenance::repairInvariant(*ctx_,plan);
        if(!result)return bad(call,progress);
        return result.value();
    }
    MaintenanceResult<MaintenanceReport> summary(const MaintenanceContext& call) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Inspect);
        if (!valid) return valid.error();
        if (invalidated_) return failure(call, ErrorCode::InvalidArgument, MaintenanceStage::Inspect);
        CallProgress progress(db_.get(), call);
        return report_;
    }
};
class Inspection final : public MetadataInspection {
    MaintenanceResult<std::unique_ptr<MetadataInspectionReader>> open(const MaintenanceContext& c, const InspectionCapture& copy) override {
        auto valid = validateMaintenanceCall(c, c.operationId, MaintenanceStage::Inspect);
        if (!valid) return valid.error();
        auto root = system::detail::openInstanceRoot(copy.root());
        if (!root) return failure(c, root.error().code, MaintenanceStage::Inspect);
        auto file = system::detail::openPrivateFile(root.value().rootFd.get(), "metadata.sqlite3", true);
        if (!file) return failure(c, file.error().code, MaintenanceStage::Inspect);
        auto db = SqliteConnection::open(copy.root() / "metadata.sqlite3", {}, SqliteOpenMode::MaintenanceCopy);
        std::optional<InstanceId> format;
        auto formatFile = system::detail::openPrivateFile(root.value().rootFd.get(), "instance.json", false);
        auto text = formatFile ? system::detail::readBoundedFile(formatFile.value().get(), 65536, ErrorCode::IntegrityError) : Result<std::string>(formatFile.error());
        if (text) {
            auto parsed = system::detail::parseFormat(text.value(), InitStage::Verify);
            if (parsed) format = parsed.value();
        }
        auto reader = std::make_unique<Reader>(c, std::move(file).value(), db ? std::move(db).value() : nullptr, format);
        auto initialized = reader->initialize(c);
        if (!initialized) return initialized.error();
        return std::unique_ptr<MetadataInspectionReader>(std::move(reader));
    }
};
}  // namespace
Result<std::unique_ptr<MetadataInspection>> makeSqliteMetadataInspection() { return std::unique_ptr<MetadataInspection>(new Inspection); }
}  // namespace anof::infrastructure::sqlite
