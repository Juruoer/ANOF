#include <anof/infrastructure/config/internal/ConfigurationText.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMaintenanceRead.h>
#include <anof/infrastructure/storage/local/LocalMaintenanceFileStore.h>
#include <anof/infrastructure/storage/local/internal/BackupEncoding.h>
#include <anof/infrastructure/storage/local/internal/MaintenanceFileTesting.h>
#include <anof/infrastructure/system/LinuxOfflineInstanceAccess.h>
#include <anof/infrastructure/system/internal/BoundedFileRead.h>
#include <anof/infrastructure/system/internal/InitializationFiles.h>
#include <anof/infrastructure/system/internal/MaintenanceFiles.h>
#include <dirent.h>
#include <fcntl.h>
#include <sodium.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <set>
namespace anof::infrastructure::storage {
namespace {
struct DirCloser {
    void operator()(DIR* p) const noexcept {
        if (p) ::closedir(p);
    }
};
namespace fs = system::detail;
namespace safe = system::maintenance;
namespace codec = maintenance;
using safe::OwnedFd;
struct Lines {
    int fd;
    std::array<char, kMaintenanceBlockBytes> bytes{};
    std::size_t begin = 0, end = 0;
    Result<std::optional<std::string>> next() {
        std::string out;
        for (;;) {
            if (begin == end) {
                auto n = ::read(fd, bytes.data(), bytes.size());
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) return fs::filesystemError(errno);
                if (!n) {
                    if (!out.empty()) return ErrorCode::IntegrityError;
                    return std::optional<std::string>{};
                }
                begin = 0;
                end = static_cast<std::size_t>(n);
            }
            char c = bytes[begin++];
            if (c == '\n') return std::optional<std::string>{ out };
            if (out.size() >= kBackupObjectLineBytes - 1) return ErrorCode::IntegrityError;
            out.push_back(c);
        }
    }
};
Result<OwnedFd> create(int parent, const char* name) {
    OwnedFd fd(::openat(parent, name, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (fd.get() < 0) return fs::filesystemError(errno);
    return fd;
}
Result<void> textFile(int parent, const char* name, const std::string& text, safe::Io* io = nullptr) {
    auto fd = create(parent, name);
    if (!fd) return fd.error();
    auto w = safe::writeAll(fd.value().get(), text.data(), text.size(), io);
    if (!w) return w.error();
    if (io ? io->sync(fd.value().get()) : ::fsync(fd.value().get())) return fs::filesystemError(errno);
    return {};
}
Result<safe::Location> objectLocation(int root, const Blob& b, bool make) {
    auto objects = safe::directory(root, "objects", make);
    if (!objects) return objects.error();
    auto prefix = safe::directory(objects.value().get(), b.id.toString().substr(0, 2), make);
    if (!prefix) return prefix.error();
    return safe::Location{ std::move(prefix).value(), b.id.toString() };
}
Result<void> copy(const MaintenanceContext& c, int source, int target, const char* name, const BackupFileDigest& expected, safe::Io* io = nullptr) {
    auto out = create(target, name);
    if (!out) return out.error();
    auto actual = safe::hashCopy(c, source, out.value().get(), io);
    if (!actual) return actual.error();
    if (actual.value().size != expected.size || actual.value().sha256 != expected.sha256) return ErrorCode::IntegrityError;
    if ((io ? io->sync(out.value().get()) : ::fsync(out.value().get())) || (io ? io->sync(target) : ::fsync(target))) return fs::filesystemError(errno);
    return {};
}
Result<std::size_t> scanFiles(int root, const std::set<std::string>& allowed) {
    std::size_t actual = 0;
    std::function<Result<void>(int, int, const std::string&)> scan = [&](int fd, int depth, const std::string& prefix) -> Result<void> {OwnedFd dup(::openat(fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC));DIR* dir=::fdopendir(::dup(dup.get()));if(!dir)return ErrorCode::InternalError;std::unique_ptr<DIR, DirCloser> owned(dir);for(;;){errno=0;auto* e=::readdir(dir);if(!e){if(errno)return ErrorCode::InternalError;break;}std::string name=e->d_name;if(name=="."||name=="..")continue;struct stat st{};if(::fstatat(fd,name.c_str(),&st,AT_SYMLINK_NOFOLLOW))return ErrorCode::InternalError;
 if(depth==0){if(name=="objects"){auto child=safe::directory(fd,name);if(!child)return child.error();auto r=scan(child.value().get(),1,"");if(!r)return r.error();}else{if(allowed.find(name)==allowed.end())return ErrorCode::IntegrityError;auto f=fs::openPrivateFile(fd,name.c_str(),false);if(!f)return f.error();}}
 else if(depth==1){if(name.size()!=2||name.find_first_not_of("0123456789abcdef")!=std::string::npos)return ErrorCode::IntegrityError;auto child=safe::directory(fd,name);if(!child)return child.error();auto r=scan(child.value().get(),2,name);if(!r)return r.error();}
 else{auto id=BlobId::parse(name);if(!id||name.substr(0,2)!=prefix)return ErrorCode::IntegrityError;auto f=fs::openPrivateFile(fd,name.c_str(),false);if(!f)return f.error();if(++actual>kBackupObjectLimit)return ErrorCode::IntegrityError;}
 }return {}; };

    auto result = scan(root, 0, "");
    if (!result) return result.error();
    return actual;
}
class Backup final : public MaintenanceBackupSource {
public:
    OperationId operation;
    std::filesystem::path path;
    OwnedFd root;
    BackupManifest value;
    std::shared_ptr<safe::Io> io;
    Backup(const MaintenanceContext& c, std::filesystem::path p, OwnedFd fd, BackupManifest m, std::shared_ptr<safe::Io> input) : operation(c.operationId), path(std::move(p)), root(std::move(fd)), value(std::move(m)), io(std::move(input)) {}
    const BackupManifest& manifest() const noexcept override { return value; }
    MaintenanceResult<Page<Blob, BlobId>> objects(const MaintenanceContext& call, PageSize size, std::optional<BlobId> after) override {
        auto valid = validateMaintenanceCall(call, operation, MaintenanceStage::Verify);
        if (!valid) return valid.error();
        auto fd = fs::openPrivateFile(root.get(), "objects.jsonl", false);
        if (!fd) return safe::failure(call, MaintenanceStage::Verify, MaintenanceError::UnsafePath);
        auto indexDigest = safe::hashCopy(call, fd.value().get(), -1, io.get());
        if (!indexDigest && call.cancelled.load()) return safe::failure(call, MaintenanceStage::Verify, MaintenanceError::Cancelled);
        if (!indexDigest || indexDigest.value().size != value.objects.file.size || indexDigest.value().sha256 != value.objects.file.sha256 || ::lseek(fd.value().get(), 0, SEEK_SET) < 0)
            return safe::failure(call, MaintenanceStage::Verify, MaintenanceError::IntegrityError);
        Lines lines{ fd.value().get() };
        Page<Blob, BlobId> page;
        for (;;) {
            if (call.cancelled.load()) return safe::failure(call, MaintenanceStage::Verify, MaintenanceError::Cancelled);
            auto line = lines.next();
            if (!line) return safe::failure(call, MaintenanceStage::Verify, MaintenanceError::IntegrityError);
            if (!line.value()) return page;
            auto json = codec::parseStrict(*line.value(), kBackupObjectLineBytes);
            if (!json) return safe::failure(call, MaintenanceStage::Verify, MaintenanceError::IntegrityError);
            auto b = codec::object(json.value());
            if (!b) return safe::failure(call, MaintenanceStage::Verify, MaintenanceError::IntegrityError);
            if (after && !(after.value() < b.value().id)) continue;
            if (page.items.size() == static_cast<std::size_t>(size.value())) {
                page.next = page.items.back().id;
                return page;
            }
            page.items.push_back(b.value());
        }
    }
};
class Files final : public MaintenanceFileStore {
    std::shared_ptr<safe::Io> io_;
    MaintenanceFailure bad(const MaintenanceContext& c, MaintenanceStage s = MaintenanceStage::Verify) { return safe::failure(c, s, c.cancelled.load() ? MaintenanceError::Cancelled : MaintenanceError::IntegrityError); }
    MaintenanceFailure failed(const MaintenanceContext& c, const Error& e, MaintenanceStage stage) {
        const auto code = c.cancelled.load() ? MaintenanceError::Cancelled : e.code == ErrorCode::StorageFull ? MaintenanceError::StorageFull
                                                                         : e.code == ErrorCode::InternalError ? MaintenanceError::IoError
                                                                                                              : MaintenanceError::IntegrityError;
        return safe::failure(c, stage, code, MaintenanceOutcome::Unknown);
    }

public:
    explicit Files(std::shared_ptr<safe::Io> io = std::make_shared<safe::Io>()) : io_(std::move(io)) {}
    MaintenanceResult<std::optional<MaintenanceFileObservation>> observe(const MaintenanceContext& c, const InstanceLease& lease, const MaintenanceFileKey& key, InspectionMode mode) override {
        if (c.cancelled.load()) return safe::failure(c, MaintenanceStage::Inspect, MaintenanceError::Cancelled);
        auto location = safe::locate(lease, key);
        if (!location) {
            if (location.error().code == ErrorCode::NotFound && std::holds_alternative<BlobId>(key)) {
                auto base = fs::instanceDirectory(lease);
                if (base && safe::directory(base.value(), "objects")) return std::optional<MaintenanceFileObservation>{};
            }
            return safe::failure(c, MaintenanceStage::Inspect, MaintenanceError::UnsafePath);
        }
        auto& loc = location.value();
        auto file = fs::openPrivateFile(loc.parent.get(), loc.name.c_str(), false);
        if (!file) {
            if (file.error().code == ErrorCode::NotFound) return std::optional<MaintenanceFileObservation>{};
            return safe::failure(c, MaintenanceStage::Inspect, MaintenanceError::UnsafePath);
        }
        auto before = safe::evidence(file.value().get());
        if (!before) return bad(c);
        std::optional<Sha256> hash;
        if (mode == InspectionMode::Full) {
            auto digest = safe::hashCopy(c, file.value().get(), -1, io_.get());
            if (!digest) return failed(c, digest.error(), MaintenanceStage::Inspect);
            hash = digest.value().sha256;
        }
        auto check = fs::openPrivateFile(loc.parent.get(), loc.name.c_str(), false);
        if (!check) return bad(c);
        auto after = safe::evidence(check.value().get());
        if (!after || !safe::same(before.value(), after.value())) return bad(c);
        return std::optional<MaintenanceFileObservation>{ { key, before.value(), hash } };
    }
    MaintenanceResult<MaintenanceActionEvidence> verifyAction(const MaintenanceContext& c, const InstanceLease& lease, const MaintenanceEvidenceBinding& binding, const MaintenanceAction& action) override {
        auto result = verifyActionImpl(c, lease, binding, action);
        if (!result) {
            auto failure = result.error();
            failure.action = action.id;
            return failure;
        }
        return result;
    }
    MaintenanceResult<MaintenanceActionEvidence> verifyActionImpl(const MaintenanceContext& c, const InstanceLease& lease, const MaintenanceEvidenceBinding& binding, const MaintenanceAction& action) {
        std::vector<MaintenanceFileKey> keys;
        InspectionMode mode = InspectionMode::Full;
        bool synchronize = true;
        std::visit([&](const auto& p) {using T=std::decay_t<decltype(p)>;if constexpr(std::is_same_v<T,RecomputeAccounting>){}else if constexpr(std::is_same_v<T,QuarantineOrphan>){keys.push_back(std::visit([](const auto& id)->MaintenanceFileKey{return id;},p.source));keys.push_back(p.destination);}else{keys.push_back(p.id);if constexpr(std::is_same_v<T,MarkBlobUnavailable>){synchronize=false;if(p.evidence.reason!=UnavailableReason::Hash)mode=InspectionMode::Quick;}if constexpr(std::is_same_v<T,TruncateUnconfirmedTail>)mode=InspectionMode::Quick;} }, action.payload);
        if (keys.empty()) return safe::failure(c, MaintenanceStage::Verify, MaintenanceError::InvalidArgument);
        if (const auto* resume = std::get_if<ResumeUpload>(&action.payload)) {
            auto upload = sqlite::readMaintenanceUpload(lease, resume->id);
            if (!upload || upload.value().generation.value() < resume->generation.value()) return bad(c);
            if (upload.value().pendingBlobId) keys.push_back(*upload.value().pendingBlobId);
        }
        MaintenanceActionEvidence out{ binding, MaintenanceEvidenceQuality::Durable, {}, action.expectedPostcondition };
        for (const auto& key : keys) {
            auto observed = observe(c, lease, key, mode);
            if (!observed) return observed.error();
            if (synchronize) {
                auto loc = safe::locate(lease, key);
                if (!loc) return bad(c);
                if (observed.value()) {
                    auto f = fs::openPrivateFile(loc.value().parent.get(), loc.value().name.c_str(), true);
                    if (!f) return bad(c);
                    auto evidence = safe::evidence(f.value().get());
                    if (!evidence || !safe::same(evidence.value(), observed.value()->evidence)) return bad(c);
                    if (io_->sync(f.value().get())) return safe::ioFailure(c, MaintenanceStage::Verify, true);
                }
                if (io_->sync(loc.value().parent.get())) return safe::ioFailure(c, MaintenanceStage::Verify, true);
            }
            auto fresh = observe(c, lease, key, mode);
            if (!fresh || bool(fresh.value()) != bool(observed.value()) ||
                (fresh.value() && (!safe::same(fresh.value()->evidence, observed.value()->evidence) || fresh.value()->sha256 != observed.value()->sha256))) return bad(c);
            out.files.push_back({ key, observed.value() ? std::optional<FileEvidence>{ observed.value()->evidence } : std::optional<FileEvidence>{}, observed.value() ? observed.value()->sha256 : std::optional<Sha256>{} });
        }
        if (!validateMaintenanceEvidence(binding, action, out)) return bad(c);
        if (const auto* resume = std::get_if<ResumeUpload>(&action.payload)) {
            auto upload = sqlite::readMaintenanceUpload(lease, resume->id);
            if (!upload || !sqlite::validateMaintenanceUploadFacts(upload.value(), *resume, out)) return bad(c);
        }
        return out;
    }
    MaintenanceResult<void> copyObject(const MaintenanceContext& c, const InstanceLease& lease, const MaintenanceDestination& d, const Blob& b, const FileEvidence& before) override {
        auto dest = safe::destination(d);
        if (!dest) return bad(c);
        auto src = safe::locate(lease, b.id);
        if (!src) return bad(c);
        auto in = fs::openPrivateFile(src.value().parent.get(), src.value().name.c_str(), false);
        if (!in) return bad(c);
        auto actual = safe::evidence(in.value().get());
        if (!actual || !safe::same(actual.value(), before)) return bad(c);
        auto out = objectLocation(dest.value()->stage.get(), b, true);
        if (!out) return bad(c);
        auto copied = copy(c, in.value().get(), out.value().parent.get(), out.value().name.c_str(), { ByteCount::from(b.size.value()).value(), b.sha256 }, io_.get());
        if (!copied) return failed(c, copied.error(), MaintenanceStage::Copy);
        return {};
    }
    MaintenanceResult<FileEvidence> quarantine(const MaintenanceContext& c, const InstanceLease& lease, const QuarantineEntry& q) override {
        if (!validateQuarantineEntry(q) || q.state != QuarantineState::Prepared) return bad(c);
        auto key = std::visit([](const auto& id) -> MaintenanceFileKey { return id; }, q.source);
        auto source = observe(c, lease, key, InspectionMode::Full);
        auto target = observe(c, lease, q.id, InspectionMode::Full);
        if (!source) return source.error();
        if (!target) return target.error();
        auto src = safe::locate(lease, key), dst = safe::locate(lease, q.id);
        if (!src || !dst) return bad(c);
        if (source.value() && target.value()) return bad(c);
        if (!source.value() && !target.value()) return bad(c);
        if (source.value()) {
            if (!safe::same(source.value()->evidence, q.evidence) || source.value()->sha256 != q.sha256) return bad(c);
            if (io_->rename(src.value().parent.get(), src.value().name.c_str(), dst.value().parent.get(), dst.value().name.c_str())) return safe::ioFailure(c, MaintenanceStage::Recover);
        } else if (target.value()->sha256 != q.sha256 || target.value()->evidence.device != q.evidence.device || target.value()->evidence.inode != q.evidence.inode || target.value()->evidence.size != q.evidence.size)
            return bad(c);
        if (io_->sync(src.value().parent.get()) || io_->sync(dst.value().parent.get())) return safe::ioFailure(c, MaintenanceStage::Recover, true);
        auto final = observe(c, lease, q.id, InspectionMode::Full);
        if (!final || !final.value()) return safe::failure(c, MaintenanceStage::Recover, MaintenanceError::PublicationOutcomeUnknown, MaintenanceOutcome::Unknown);
        return final.value()->evidence;
    }
    MaintenanceResult<void> truncateTail(const MaintenanceContext& c, const InstanceLease& lease, const TruncateUnconfirmedTail& p) override {
        auto loc = safe::locate(lease, p.id);
        if (!loc) return bad(c);
        auto fd = fs::openPrivateFile(loc.value().parent.get(), loc.value().name.c_str(), true);
        if (!fd) return bad(c);
        auto before = safe::evidence(fd.value().get());
        if (!before || !safe::same(before.value(), p.before) || p.confirmed.value() > before.value().size.value()) return bad(c);
        if (c.cancelled.load()) return safe::failure(c, MaintenanceStage::Recover, MaintenanceError::Cancelled);
        if (io_->truncate(fd.value().get(), p.confirmed.value())) return safe::ioFailure(c, MaintenanceStage::Recover, true);
        if (io_->sync(fd.value().get())) return safe::ioFailure(c, MaintenanceStage::Recover, true);
        return {};
    }
    MaintenanceResult<BackupFileDigest> writeConfiguration(const MaintenanceContext& c, const MaintenanceDestination& d, const InstanceConfiguration& value) override {
        auto dest = safe::destination(d);
        if (!dest) return bad(c);
        auto text = config::detail::serializeConfiguration(value);
        if (!text) return bad(c);
        auto saved = textFile(dest.value()->stage.get(), "config.toml", text.value(), io_.get());
        if (!saved) return failed(c, saved.error(), MaintenanceStage::Copy);
        auto file = fs::openPrivateFile(dest.value()->stage.get(), "config.toml", false);
        if (!file) return bad(c);
        auto hash = safe::hashCopy(c, file.value().get(), -1, io_.get());
        if (!hash) return bad(c);
        return hash.value();
    }
    MaintenanceResult<void> appendObjectIndex(const MaintenanceContext& c, const MaintenanceDestination& d, const std::vector<Blob>& page) override {
        auto dest = safe::destination(d);
        if (!dest || page.size() > 200 || dest.value()->finishedIndex || dest.value()->failed) return bad(c);
        auto next = dest.value()->index;
        auto count = dest.value()->indexCount;
        auto bytes = dest.value()->indexBytes;
        std::string text;
        for (const auto& b : page) {
            if (!next.append(b)) return bad(c);
            text += codec::encode(b).dump() + "\n";
            count = count.add(1).value();
            auto sum = bytes.add(b.size.value());
            if (!sum) return bad(c);
            bytes = sum.value();
        }
        OwnedFd fd(::openat(dest.value()->stage.get(), "objects.jsonl", O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (fd.get() < 0) {
            if (errno != EEXIST) return safe::ioFailure(c, MaintenanceStage::Copy);
            auto old = fs::openPrivateFile(dest.value()->stage.get(), "objects.jsonl", true);
            if (!old) return bad(c);
            fd = std::move(old).value();
            if (::lseek(fd.get(), 0, SEEK_END) < 0) return bad(c);
        }
        auto written = safe::writeAll(fd.get(), text.data(), text.size(), io_.get());
        if (!written) {
            dest.value()->failed = true;
            return safe::ioFailure(c, MaintenanceStage::Copy, true);
        }
        dest.value()->index = next;
        dest.value()->indexCount = count;
        dest.value()->indexBytes = bytes;
        return {};
    }
    MaintenanceResult<BackupObjectIndex> finishObjectIndex(const MaintenanceContext& c, const MaintenanceDestination& d) override {
        auto dest = safe::destination(d);
        if (!dest || dest.value()->failed) return bad(c);
        if (dest.value()->finishedIndex) return *dest.value()->finishedIndex;
        auto empty = appendObjectIndex(c, d, {});
        if (!empty) return empty.error();
        auto fd = fs::openPrivateFile(dest.value()->stage.get(), "objects.jsonl", true);
        if (!fd) return bad(c);
        if (io_->sync(fd.value().get())) return safe::ioFailure(c, MaintenanceStage::Copy, true);
        auto hash = safe::hashCopy(c, fd.value().get(), -1, io_.get());
        if (!hash) return bad(c);
        BackupObjectIndex out{ hash.value(), dest.value()->indexCount, dest.value()->indexBytes };
        dest.value()->finishedIndex = out;
        return out;
    }
    MaintenanceResult<std::unique_ptr<MaintenanceBackupSource>> openBackup(const MaintenanceContext&, const std::filesystem::path&) override;
    MaintenanceResult<void> sealBackup(const MaintenanceContext&, const MaintenanceDestination&, const BackupManifest&) override;
    MaintenanceResult<void> copyBackup(const MaintenanceContext&, MaintenanceBackupSource&, const MaintenanceDestination&) override;
    MaintenanceResult<void> sealRestoredInstance(const MaintenanceContext&, const MaintenanceDestination&, InstanceId) override;
    MaintenanceResult<void> publish(const MaintenanceContext&, const MaintenanceDestination&) override;
    MaintenanceResult<void> verifyOriginal(const MaintenanceContext&, const std::filesystem::path&, const MaintenanceFingerprint&) override;
    MaintenanceResult<void> preserveOriginal(const MaintenanceContext&, const InstanceLease&, const MaintenanceDestination&, const MaintenanceFingerprint&) override;
    MaintenanceResult<std::unique_ptr<MaintenanceInventory>> inventory(const MaintenanceContext&, const InstanceLease&, InspectionMode) override;
};
MaintenanceResult<std::unique_ptr<MaintenanceBackupSource>> Files::openBackup(const MaintenanceContext& c, const std::filesystem::path& path) {
    auto valid = validateMaintenanceCall(c, c.operationId, MaintenanceStage::Verify);
    if (!valid) return valid.error();
    auto root = fs::openInstanceRoot(path);
    if (!root) return safe::failure(c, MaintenanceStage::Verify, MaintenanceError::UnsafePath);
    auto read = [&](const char* name, std::size_t limit) -> Result<std::string> {auto fd=fs::openPrivateFile(root.value().rootFd.get(),name,false);if(!fd)return fd.error();return fs::readBoundedFile(fd.value().get(),limit,ErrorCode::IntegrityError); };
    auto text = read("manifest.json", kBackupManifestBytes), complete = read("COMPLETE", 65);
    if (!text || !complete) return bad(c);
    auto json = codec::parseStrict(text.value(), kBackupManifestBytes);
    if (!json) return bad(c);
    auto manifest = codec::manifest(json.value());
    if (!manifest) return bad(c);
    std::array<unsigned char, 32> hash{};
    crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(text.value().data()), text.value().size());
    if (complete.value() != Sha256(hash).toString() + "\n") return bad(c);
    for (auto pair : { std::pair<const char*, BackupFileDigest>{ "metadata.sqlite3", manifest.value().database }, { "config.toml", manifest.value().configuration }, { "objects.jsonl", manifest.value().objects.file } }) {
        auto file = fs::openPrivateFile(root.value().rootFd.get(), pair.first, false);
        if (!file) return bad(c);
        auto digest = safe::hashCopy(c, file.value().get(), -1, io_.get());
        if (!digest || digest.value().size != pair.second.size || digest.value().sha256 != pair.second.sha256) return bad(c);
    }
    auto objectsDirectory = safe::directory(root.value().rootFd.get(), "objects");
    if (!objectsDirectory) return bad(c);
    auto index = fs::openPrivateFile(root.value().rootFd.get(), "objects.jsonl", false);
    if (!index) return bad(c);
    Lines lines{ index.value().get() };
    BackupIndexValidator validator;
    std::size_t expected = 0;
    for (;;) {
        auto line = lines.next();
        if (!line) return bad(c);
        if (!line.value()) break;
        auto parsed = codec::parseStrict(*line.value(), kBackupObjectLineBytes);
        if (!parsed) return bad(c);
        auto blob = codec::object(parsed.value());
        if (!blob || !validator.append(blob.value())) return bad(c);
        auto loc = objectLocation(root.value().rootFd.get(), blob.value(), false);
        if (!loc) return bad(c);
        auto file = fs::openPrivateFile(loc.value().parent.get(), loc.value().name.c_str(), false);
        if (!file) return bad(c);
        auto digest = safe::hashCopy(c, file.value().get(), -1, io_.get());
        if (!digest || digest.value().size.value() != blob.value().size.value() || digest.value().sha256 != blob.value().sha256) return bad(c);
        ++expected;
    }
    if (!validator.finish(manifest.value().objects)) return bad(c);
    auto actual = scanFiles(root.value().rootFd.get(), { "metadata.sqlite3", "config.toml", "manifest.json", "objects.jsonl", "COMPLETE" });
    if (!actual || actual.value() != expected) return bad(c);
    return std::unique_ptr<MaintenanceBackupSource>(new Backup(c, path, std::move(root.value().rootFd), manifest.value(), io_));
}
MaintenanceResult<void> Files::sealBackup(const MaintenanceContext& c, const MaintenanceDestination& d, const BackupManifest& manifest) {
    auto dest = safe::destination(d);
    if (!dest || !validateBackupManifest(manifest) || dest.value()->sealed) return bad(c);
    auto index = finishObjectIndex(c, d);
    if (!index) return index.error();
    if (index.value().file.sha256 != manifest.objects.file.sha256 || index.value().file.size != manifest.objects.file.size || index.value().count != manifest.objects.count || index.value().totalBytes != manifest.objects.totalBytes) return bad(c);
    auto objects = safe::directory(dest.value()->stage.get(), "objects", true);
    if (!objects) return bad(c);
    auto text = codec::encode(manifest).dump() + "\n";
    if (text.size() > kBackupManifestBytes) return bad(c);
    auto written = textFile(dest.value()->stage.get(), "manifest.json", text, io_.get());
    if (!written) return bad(c);
    std::array<unsigned char, 32> hash{};
    crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(text.data()), text.size());
    written = textFile(dest.value()->stage.get(), "COMPLETE", Sha256(hash).toString() + "\n", io_.get());
    if (!written) return bad(c);
    auto checked = openBackup(c, d.staging());
    if (!checked) return checked.error();
    if (io_->sync(objects.value().get()) || io_->sync(dest.value()->stage.get())) return safe::ioFailure(c, MaintenanceStage::Verify, true);
    dest.value()->sealed = true;
    return {};
}
MaintenanceResult<void> Files::copyBackup(const MaintenanceContext& c, MaintenanceBackupSource& source, const MaintenanceDestination& d) {
    auto* backup = dynamic_cast<Backup*>(&source);
    auto dest = safe::destination(d);
    if (!backup || !dest) return bad(c);
    auto valid = validateMaintenanceCall(c, backup->operation, MaintenanceStage::Verify);
    if (!valid) return valid.error();
    auto db = fs::openPrivateFile(backup->root.get(), "metadata.sqlite3", false);
    if (!db) return bad(c);
    auto work = [&]() -> MaintenanceResult<void> {
        auto copied = copy(c, db.value().get(), dest.value()->stage.get(), "metadata.sqlite3", backup->value.database, io_.get());
        if (!copied) return failed(c, copied.error(), MaintenanceStage::Copy);
        std::optional<BlobId> after;
        do {
            auto page = backup->objects(c, PageSize::from(200).value(), after);
            if (!page) return page.error();
            for (const auto& b : page.value().items) {
                auto from = objectLocation(backup->root.get(), b, false), to = objectLocation(dest.value()->stage.get(), b, true);
                if (!from || !to) return bad(c);
                auto input = fs::openPrivateFile(from.value().parent.get(), from.value().name.c_str(), false);
                if (!input) return bad(c);
                auto r = copy(c, input.value().get(), to.value().parent.get(), to.value().name.c_str(), { ByteCount::from(b.size.value()).value(), b.sha256 }, io_.get());
                if (!r) return failed(c, r.error(), MaintenanceStage::Copy);
            }
            after = page.value().next;
        } while (after);
        auto index = fs::openPrivateFile(backup->root.get(), "objects.jsonl", false);
        if (!index) return bad(c);
        auto copiedIndex = copy(c, index.value().get(), dest.value()->stage.get(), "objects.jsonl", backup->value.objects.file, io_.get());
        if (!copiedIndex) return failed(c, copiedIndex.error(), MaintenanceStage::Copy);
        dest.value()->restoreManifest = backup->value;
        return {};
    };
    auto result = work();
    if (!result) {
        auto error = result.error();
        error.outcome = MaintenanceOutcome::Unknown;
        dest.value()->failed = true;
        return error;
    }
    return {};
}
MaintenanceResult<void> Files::sealRestoredInstance(const MaintenanceContext& c, const MaintenanceDestination& d, InstanceId id) {
    auto dest = safe::destination(d);
    if (!dest || dest.value()->sealed || dest.value()->failed || !dest.value()->restorePrepared || *dest.value()->restorePrepared != id || !dest.value()->restoreManifest || dest.value()->restoreManifest->instance != id) return bad(c);
    auto index = fs::openPrivateFile(dest.value()->stage.get(), "objects.jsonl", false);
    if (!index) return bad(c);
    auto indexHash = safe::hashCopy(c, index.value().get(), -1, io_.get());
    const auto& expected = dest.value()->restoreManifest->objects;
    if (!indexHash || indexHash.value().size != expected.file.size || indexHash.value().sha256 != expected.file.sha256 || ::lseek(index.value().get(), 0, SEEK_SET) < 0) return bad(c);
    Lines lines{ index.value().get() };
    BackupIndexValidator validator;
    for (;;) {
        auto line = lines.next();
        if (!line) return bad(c);
        if (!line.value()) break;
        auto json = codec::parseStrict(*line.value(), kBackupObjectLineBytes);
        if (!json) return bad(c);
        auto blob = codec::object(json.value());
        if (!blob || !validator.append(blob.value())) return bad(c);
        auto loc = objectLocation(dest.value()->stage.get(), blob.value(), false);
        if (!loc) return bad(c);
        auto file = fs::openPrivateFile(loc.value().parent.get(), loc.value().name.c_str(), false);
        if (!file) return bad(c);
        auto hash = safe::hashCopy(c, file.value().get(), -1, io_.get());
        if (!hash || hash.value().sha256 != blob.value().sha256 || hash.value().size.value() != blob.value().size.value()) return bad(c);
        if (io_->sync(file.value().get()) || io_->sync(loc.value().parent.get())) return safe::ioFailure(c, MaintenanceStage::Verify, true);
    }
    if (!validator.finish(expected)) return bad(c);
    auto count = scanFiles(dest.value()->stage.get(), { "metadata.sqlite3", "config.toml", "objects.jsonl" });
    if (!count || count.value() != static_cast<std::size_t>(expected.count.value())) return bad(c);
    auto config = fs::openPrivateFile(dest.value()->stage.get(), "config.toml", false);
    if (!config) return bad(c);
    auto text = fs::readBoundedFile(config.value().get(), 65536, ErrorCode::IntegrityError);
    if (!text || !config::detail::parseConfiguration(text.value(), true)) return bad(c);
    auto metadata = fs::openPrivateFile(dest.value()->stage.get(), "metadata.sqlite3", false);
    if (!metadata) return bad(c);
    if (io_->sync(metadata.value().get()) || io_->sync(config.value().get())) return safe::ioFailure(c, MaintenanceStage::Verify, true);
    if (::unlinkat(dest.value()->stage.get(), "objects.jsonl", 0)) return safe::ioFailure(c, MaintenanceStage::Verify, true);
    auto anof = safe::directory(dest.value()->stage.get(), ".anof", true);
    if (!anof) return bad(c);
    auto objects = safe::directory(dest.value()->stage.get(), "objects", true);
    if (!objects) return bad(c);
    // shard 的目录项属于 objects；必须先同步这一层，再同步其父目录。
    if (io_->sync(objects.value().get())) return safe::ioFailure(c, MaintenanceStage::Verify, true);
    for (auto name : { "metadata.sqlite3", "config.toml", "objects" })
        if (io_->rename(dest.value()->stage.get(), name, anof.value().get(), name)) return safe::ioFailure(c, MaintenanceStage::Verify, true);
    for (auto name : { "uploads", "quarantine" }) {
        auto sub = safe::directory(anof.value().get(), name, true);
        if (!sub || io_->sync(sub.value().get())) return safe::ioFailure(c, MaintenanceStage::Verify, true);
    }
    for (auto pair : { std::pair<const char*, std::string>{ "instance.json", fs::formatText({ id, c.operationId }) }, { "restore-operation", c.operationId.toString() + "\n" }, { "instance.lock", "" } }) {
        auto r = textFile(anof.value().get(), pair.first, pair.second, io_.get());
        if (!r) return bad(c);
    }
    if (io_->sync(anof.value().get()) || io_->sync(dest.value()->stage.get())) return safe::ioFailure(c, MaintenanceStage::Verify, true);
    dest.value()->sealed = true;
    return {};
}
MaintenanceResult<void> Files::publish(const MaintenanceContext& c, const MaintenanceDestination& d) {
    auto dest = safe::destination(d);
    if (!dest || !dest.value()->sealed) return bad(c, MaintenanceStage::Publish);
    if (c.cancelled.load()) return safe::failure(c, MaintenanceStage::Publish, MaintenanceError::Cancelled);
    if (io_->rename(dest.value()->parent.get(), d.staging().filename().c_str(), dest.value()->parent.get(), d.target().filename().c_str())) {
        if (errno == EEXIST) return safe::failure(c, MaintenanceStage::Publish, MaintenanceError::AlreadyExists);
        return safe::ioFailure(c, MaintenanceStage::Publish);
    }
    if (io_->sync(dest.value()->parent.get())) {
        auto failure = safe::ioFailure(c, MaintenanceStage::Publish, true);
        failure.artifact = d.target();
        return failure;
    }
    return {};
}
MaintenanceResult<void> Files::preserveOriginal(const MaintenanceContext& c, const InstanceLease& lease, const MaintenanceDestination& d, const MaintenanceFingerprint& fingerprint) {
    auto dest = safe::destination(d);
    if (!dest) return bad(c);
    auto access = system::makeLinuxOfflineInstanceAccess();
    if (!access) return bad(c);
    auto capture = access.value()->captureInspection(c, lease, d.staging());
    if (!capture) return capture.error();
    if (capture.value()->fingerprint().metadata != fingerprint.metadata || capture.value()->fingerprint().inventory != fingerprint.inventory) return safe::failure(c, MaintenanceStage::PreserveOriginal, MaintenanceError::StalePlan);
    auto input = fs::openInstanceRoot(capture.value()->root());
    if (!input) return bad(c);
    codec::Json manifest = codec::Json::object();
    for (auto name : { "metadata.sqlite3", "metadata.sqlite3-wal", "metadata.sqlite3-journal", "metadata.sqlite3-shm.raw", "config.toml", "instance.json" }) {
        auto file = fs::openPrivateFile(input.value().rootFd.get(), name, false);
        if (!file) {
            if (file.error().code == ErrorCode::NotFound) continue;
            return bad(c);
        }
        auto hash = safe::hashCopy(c, file.value().get(), -1, io_.get());
        if (!hash) return bad(c);
        auto saved = copy(c, file.value().get(), dest.value()->stage.get(), std::string(name) == "metadata.sqlite3-shm.raw" ? "metadata.sqlite3-shm" : name, hash.value(), io_.get());
        if (!saved) return failed(c, saved.error(), MaintenanceStage::Copy);
        manifest[std::string(name) == "metadata.sqlite3-shm.raw" ? "metadata.sqlite3-shm" : name] = { { "size", std::to_string(hash.value().size.value()) }, { "sha256", hash.value().sha256.toString() } };
    }
    capture.value().reset();
    auto text = codec::Json{ { "format", 1 }, { "metadata", fingerprint.metadata.toString() }, { "inventory", fingerprint.inventory.toString() }, { "files", manifest } }.dump() + "\n";
    auto saved = textFile(dest.value()->stage.get(), "raw-manifest.json", text, io_.get());
    if (!saved) return failed(c, saved.error(), MaintenanceStage::Copy);
    std::array<unsigned char, 32> hash{};
    crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(text.data()), text.size());
    saved = textFile(dest.value()->stage.get(), "COMPLETE", Sha256(hash).toString() + "\n", io_.get());
    if (!saved || io_->sync(dest.value()->stage.get())) return safe::ioFailure(c, MaintenanceStage::PreserveOriginal, true);
    dest.value()->sealed = true;
    return publish(c, d);
}
MaintenanceResult<void> Files::verifyOriginal(const MaintenanceContext& c, const std::filesystem::path& path, const MaintenanceFingerprint& expected) {
    if (c.cancelled.load()) return safe::failure(c, MaintenanceStage::PreserveOriginal, MaintenanceError::Cancelled);
    auto root = fs::openInstanceRoot(path);
    if (!root) return bad(c, MaintenanceStage::PreserveOriginal);
    auto manifestFile = fs::openPrivateFile(root.value().rootFd.get(), "raw-manifest.json", false);
    auto completeFile = fs::openPrivateFile(root.value().rootFd.get(), "COMPLETE", false);
    if (!manifestFile || !completeFile) return bad(c, MaintenanceStage::PreserveOriginal);
    auto text = fs::readBoundedFile(manifestFile.value().get(), 16384, ErrorCode::IntegrityError);
    auto marker = fs::readBoundedFile(completeFile.value().get(), 65, ErrorCode::IntegrityError);
    if (!text || !marker) return bad(c, MaintenanceStage::PreserveOriginal);
    std::array<unsigned char, 32> sum{};
    crypto_hash_sha256(sum.data(), reinterpret_cast<const unsigned char*>(text.value().data()), text.value().size());
    if (marker.value() != Sha256(sum).toString() + "\n") return bad(c, MaintenanceStage::PreserveOriginal);
    auto decoded = codec::parseStrict(text.value(), 16384);
    if (!decoded) return bad(c, MaintenanceStage::PreserveOriginal);
    try {
        auto& doc = decoded.value();
        if (!doc.is_object() || doc.size() != 4 || doc.at("format") != 1 || doc.at("metadata") != expected.metadata.toString() || doc.at("inventory") != expected.inventory.toString() || !doc.at("files").is_object()) return bad(c, MaintenanceStage::PreserveOriginal);
        const auto& entries = doc.at("files");
        std::set<std::string> allowed = { "raw-manifest.json", "COMPLETE" };
        crypto_hash_sha256_state fingerprint;
        crypto_hash_sha256_init(&fingerprint);
        for (const char* name : { "metadata.sqlite3", "metadata.sqlite3-wal", "metadata.sqlite3-journal", "metadata.sqlite3-shm", "config.toml", "instance.json" }) {
            std::string record;
            if (!entries.contains(name)) {
                if (std::string(name).find("sqlite3-") == std::string::npos) return bad(c, MaintenanceStage::PreserveOriginal);
                record = std::string(name) + ":absent\n";
            } else {
                const auto& entry = entries.at(name);
                if (!entry.is_object() || entry.size() != 2 || !entry.at("size").is_string() || !entry.at("sha256").is_string()) return bad(c, MaintenanceStage::PreserveOriginal);
                auto size = parseTransferDecimal(entry.at("size").get<std::string>());
                auto sha = Sha256::parse(entry.at("sha256").get<std::string>());
                auto file = fs::openPrivateFile(root.value().rootFd.get(), name, false);
                if (!size || !sha || !file) return bad(c, MaintenanceStage::PreserveOriginal);
                auto actual = safe::hashCopy(c, file.value().get(), -1, io_.get());
                if (!actual) return failed(c, actual.error(), MaintenanceStage::PreserveOriginal);
                if (actual.value().size != size.value() || actual.value().sha256 != sha.value()) return bad(c, MaintenanceStage::PreserveOriginal);
                allowed.insert(name);
                record = std::string(name) + ":" + std::to_string(size.value().value()) + ":" + sha.value().toString() + "\n";
            }
            crypto_hash_sha256_update(&fingerprint, reinterpret_cast<const unsigned char*>(record.data()), record.size());
        }
        if (entries.size() + 2 != allowed.size()) return bad(c, MaintenanceStage::PreserveOriginal);
        OwnedFd scanFd(::openat(root.value().rootFd.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (scanFd.get() < 0) return bad(c, MaintenanceStage::PreserveOriginal);
        std::unique_ptr<DIR, DirCloser> directory(::fdopendir(::dup(scanFd.get())));
        if (!directory) return bad(c, MaintenanceStage::PreserveOriginal);
        std::size_t count = 0;
        for (;;) {
            errno = 0;
            auto* entry = ::readdir(directory.get());
            if (!entry) {
                if (errno) return bad(c, MaintenanceStage::PreserveOriginal);
                break;
            }
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            if (!allowed.count(name) || ++count > allowed.size()) return bad(c, MaintenanceStage::PreserveOriginal);
        }
        if (count != allowed.size()) return bad(c, MaintenanceStage::PreserveOriginal);
        crypto_hash_sha256_final(&fingerprint, sum.data());
        if (Sha256(sum) != expected.metadata) return bad(c, MaintenanceStage::PreserveOriginal);
        return {};
    } catch (const std::exception&) {
        return bad(c, MaintenanceStage::PreserveOriginal);
    }
}
class Inventory final : public MaintenanceInventory {
    OperationId operation_;
    bool invalidated_ = false;
    const InstanceLease& lease_;
    InspectionMode mode_;
    Files files_;
    int area_ = 0;
    OwnedFd areaFd_, prefixFd_;
    std::unique_ptr<DIR, DirCloser> entries_, prefixes_;
    std::string prefix_;
    MaintenanceFinding unsafe() { return { FindingCode::UnsafeEntry, FindingSeverity::Error, MaintenanceResource::Instance, {}, {}, false }; }
    Result<void> openArea() {
        const char* names[] = { "objects", "uploads", "quarantine" };
        auto base = fs::instanceDirectory(lease_);
        if (!base) return base.error();
        auto dir = safe::directory(base.value(), names[area_]);
        if (!dir) return dir.error();
        areaFd_ = std::move(dir).value();
        auto* stream = ::fdopendir(::dup(areaFd_.get()));
        if (!stream) return ErrorCode::InternalError;
        if (area_ == 0)
            prefixes_.reset(stream);
        else
            entries_.reset(stream);
        return {};
    }

public:
    Inventory(const MaintenanceContext& c, const InstanceLease& lease, InspectionMode mode, std::shared_ptr<safe::Io> io) : operation_(c.operationId), lease_(lease), mode_(mode), files_(std::move(io)) {}
    MaintenanceResult<std::vector<InventoryItem>> next(const MaintenanceContext& call, PageSize size) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Inspect);
        if (!valid) return valid.error();
        if (invalidated_) return safe::failure(call, MaintenanceStage::Inspect, MaintenanceError::InvalidArgument);
        // 入口拒绝不改变游标；开始扫描后任何异常退出要求重开，不能跳过已消费项。
        invalidated_ = true;
        std::vector<InventoryItem> page;
        if (!fs::validateInstanceLease(lease_)) return safe::failure(call, MaintenanceStage::Inspect, MaintenanceError::UnsafePath);
        while (page.size() < static_cast<std::size_t>(size.value()) && area_ < 3) {
            if (call.cancelled.load()) return safe::failure(call, MaintenanceStage::Inspect, MaintenanceError::Cancelled);
            if (!entries_ && !prefixes_) {
                auto r = openArea();
                if (!r) {
                    page.push_back(unsafe());
                    ++area_;
                    continue;
                }
            }
            if (area_ == 0 && !entries_) {
                errno = 0;
                auto* e = ::readdir(prefixes_.get());
                if (!e) {
                    if (errno) return safe::ioFailure(call, MaintenanceStage::Inspect);
                    prefixes_.reset();
                    ++area_;
                    continue;
                }
                prefix_ = e->d_name;
                if (prefix_ == "." || prefix_ == "..") continue;
                if (prefix_.size() != 2 || prefix_.find_first_not_of("0123456789abcdef") != std::string::npos) {
                    page.push_back(unsafe());
                    continue;
                }
                auto sub = safe::directory(areaFd_.get(), prefix_);
                if (!sub) {
                    page.push_back(unsafe());
                    continue;
                }
                prefixFd_ = std::move(sub).value();
                auto* stream = ::fdopendir(::dup(prefixFd_.get()));
                if (!stream) return safe::ioFailure(call, MaintenanceStage::Inspect);
                entries_.reset(stream);
            }
            errno = 0;
            auto* e = ::readdir(entries_.get());
            if (!e) {
                if (errno) return safe::ioFailure(call, MaintenanceStage::Inspect);
                entries_.reset();
                if (area_ != 0) ++area_;
                continue;
            }
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            std::optional<MaintenanceFileKey> key;
            if (area_ == 0) {
                auto id = BlobId::parse(name);
                if (id && name.substr(0, 2) == prefix_) key = id.value();
            } else if (area_ == 1) {
                if (name.size() == 41 && name.substr(36) == ".part") {
                    auto id = UploadId::parse(name.substr(0, 36));
                    if (id) key = id.value();
                }
            } else {
                auto id = QuarantineId::parse(name);
                if (id) key = id.value();
            }
            if (!key) {
                page.push_back(unsafe());
                continue;
            }
            auto observed = files_.observe(call, lease_, *key, mode_);
            if (!observed && observed.error().code == MaintenanceError::Cancelled) return observed.error();
            if (!observed || !observed.value())
                page.push_back(unsafe());
            else
                page.push_back(*observed.value());
        }
        invalidated_ = false;
        return page;
    }
};
MaintenanceResult<std::unique_ptr<MaintenanceInventory>> Files::inventory(const MaintenanceContext& c, const InstanceLease& lease, InspectionMode mode) {
    auto valid = validateMaintenanceCall(c, c.operationId, MaintenanceStage::Inspect);
    if (!valid) return valid.error();
    if (!fs::validateInstanceLease(lease)) return safe::failure(c, MaintenanceStage::Inspect, MaintenanceError::UnsafePath);
    return std::unique_ptr<MaintenanceInventory>(new Inventory(c, lease, mode, io_));
}
}  // namespace
Result<std::unique_ptr<MaintenanceFileStore>> detail::makeLocalMaintenanceFileStore(std::shared_ptr<system::maintenance::Io> io) {
    if (!io) return ErrorCode::InvalidArgument;
    return std::unique_ptr<MaintenanceFileStore>(new Files(std::move(io)));
}
Result<std::unique_ptr<MaintenanceFileStore>> makeLocalMaintenanceFileStore() { return std::unique_ptr<MaintenanceFileStore>(new Files); }
}  // namespace anof::infrastructure::storage
