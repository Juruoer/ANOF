#include <anof/infrastructure/system/LinuxOfflineInstanceAccess.h>
#include <anof/infrastructure/system/internal/MaintenanceFiles.h>
#include <dirent.h>
#include <fcntl.h>
#include <sodium.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
namespace anof::infrastructure::system {
namespace {
struct DirCloser {
    void operator()(DIR* p) const noexcept {
        if (p) ::closedir(p);
    }
};
using namespace maintenance;
bool overlap(const std::filesystem::path& a, const std::filesystem::path& b) {
    auto x = a.lexically_normal(), y = b.lexically_normal();
    auto prefix = [](const auto& p, const auto& q) {auto i=p.begin(),j=q.begin();for(;i!=p.end()&&j!=q.end();++i,++j)if(*i!=*j)return false;return i==p.end(); };
    return prefix(x, y) || prefix(y, x);
}
class Capture final : public InspectionCapture {
    std::filesystem::path path_;
    MaintenanceFingerprint fingerprint_;
    Sha256 configuration_;
    OwnedFd owned_;

public:
    Capture(std::filesystem::path p, MaintenanceFingerprint f, Sha256 configuration, OwnedFd fd) : path_(std::move(p)), fingerprint_(std::move(f)), configuration_(configuration), owned_(std::move(fd)) {}
    ~Capture() override {
        try {
            auto current = detail::openInstanceRoot(path_);
            if (!current) return;
            struct stat a{}, b{};
            if (::fstat(owned_.get(), &a) || ::fstat(current.value().rootFd.get(), &b) || a.st_dev != b.st_dev || a.st_ino != b.st_ino) return;
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        } catch (...) {
        }
    }
    const std::filesystem::path& root() const noexcept override { return path_; }
    const MaintenanceFingerprint& fingerprint() const noexcept override { return fingerprint_; }
    const Sha256& configurationFingerprint() const noexcept override { return configuration_; }
};
// 每页只保留最小的200项；重复扫描以换取固定内存，不跟随任何链接。
Result<Sha256> inventoryFingerprint(const MaintenanceContext& c, int root) {
    crypto_hash_sha256_state hash;
    crypto_hash_sha256_init(&hash);
    std::string after;
    for (;;) {
        std::vector<std::pair<std::string, std::string>> page;
        std::function<Result<void>(int, const std::string&, int)> visit = [&](int parent, const std::string& prefix, int depth) -> Result<void> {
            OwnedFd duplicate(::openat(parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (duplicate.get() < 0) return detail::filesystemError(errno);
            DIR* dir = ::fdopendir(::dup(duplicate.get()));
            if (!dir) return detail::filesystemError(errno);
            std::unique_ptr<DIR, DirCloser> owned(dir);
            for (;;) {
                if (c.cancelled.load()) return ErrorCode::InvalidArgument;
                errno = 0;
                auto* item = ::readdir(dir);
                if (!item) {
                    if (errno) return detail::filesystemError(errno);
                    break;
                }
                std::string name = item->d_name;
                if (name == "." || name == "..") continue;
                struct stat st{};
                if (::fstatat(parent, name.c_str(), &st, AT_SYMLINK_NOFOLLOW)) return detail::filesystemError(errno);
                auto key = prefix + name;
                if (key > after) {
                    auto row = key + "\t" + std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" + std::to_string(st.st_mode) + ":" + std::to_string(st.st_uid) + ":" + std::to_string(st.st_nlink) + ":" + std::to_string(st.st_size) + ":" + std::to_string(st.st_mtim.tv_sec) + ":" + std::to_string(st.st_mtim.tv_nsec) + ":" + std::to_string(st.st_ctim.tv_sec) + ":" + std::to_string(st.st_ctim.tv_nsec) + "\n";
                    auto at = std::lower_bound(page.begin(), page.end(), key, [](const auto& pair, const std::string& k) { return pair.first < k; });
                    if (page.size() < 200 || at != page.end()) {
                        page.insert(at, { key, row });
                        if (page.size() > 200) page.pop_back();
                    }
                }
                if (S_ISDIR(st.st_mode)) {
                    if (depth >= 3) return ErrorCode::IntegrityError;
                    auto child = directory(parent, name);
                    if (!child) return child.error();
                    auto r = visit(child.value().get(), key + "/", depth + 1);
                    if (!r) return r.error();
                }
            }
            return {};
        };
        auto scanned = visit(root, "", 0);
        if (!scanned) return scanned.error();
        if (page.empty()) break;
        for (const auto& p : page) crypto_hash_sha256_update(&hash, reinterpret_cast<const unsigned char*>(p.second.data()), p.second.size());
        after = page.back().first;
    }
    std::array<std::uint8_t, 32> bytes{};
    crypto_hash_sha256_final(&hash, bytes.data());
    return Sha256(bytes);
}
class Access final : public OfflineInstanceAccess {
    MaintenanceResult<std::unique_ptr<InstanceLease>> acquireExisting(const MaintenanceContext& c, const std::filesystem::path& root) override {
        if (c.cancelled.load()) return failure(c, MaintenanceStage::AcquireLocks, MaintenanceError::Cancelled);
        auto dirs = detail::openInstanceDirectories(root);
        if (!dirs) return failure(c, MaintenanceStage::AcquireLocks, MaintenanceError::UnsafePath);
        auto lease = detail::acquireInstanceLease(std::move(dirs).value(), false);
        if (!lease) return failure(c, MaintenanceStage::AcquireLocks, lease.error().code == ErrorCode::Conflict ? MaintenanceError::Busy : MaintenanceError::UnsafePath);
        return std::move(lease).value();
    }
    MaintenanceResult<std::unique_ptr<InspectionCapture>> captureInspection(const MaintenanceContext& c, const InstanceLease& lease, const std::filesystem::path& requestedWork) override {
        const auto work = requestedWork.empty() ? std::filesystem::temp_directory_path() : requestedWork;
        auto valid = detail::validateInstanceLease(lease);
        if (!valid) return failure(c, MaintenanceStage::Capture, MaintenanceError::UnsafePath);
        if (overlap(lease.root(), work / ("inspection-" + c.operationId.toString()))) return failure(c, MaintenanceStage::Capture, MaintenanceError::UnsafePath);
        auto parent = detail::openControlledDirectory(work);
        if (!parent) return failure(c, MaintenanceStage::Capture, MaintenanceError::UnsafePath);
        auto input = detail::instanceDirectory(lease).value();
        auto before = inventoryFingerprint(c, input);
        if (!before) return failure(c, MaintenanceStage::Capture, MaintenanceError::CheckIncomplete);
        std::string pattern = "/proc/self/fd/" + std::to_string(parent.value().rootFd.get()) + "/inspection-" + c.operationId.toString() + "-XXXXXX";
        std::vector<char> temporary(pattern.begin(), pattern.end());
        temporary.push_back('\0');
        if (!::mkdtemp(temporary.data())) return ioFailure(c, MaintenanceStage::Capture);
        auto path = parent.value().root / std::filesystem::path(temporary.data()).filename();
        auto output = directory(parent.value().rootFd.get(), path.filename());
        if (!output) return failure(c, MaintenanceStage::Capture, MaintenanceError::UnsafePath);
        crypto_hash_sha256_state hash;
        crypto_hash_sha256_init(&hash);
        crypto_hash_sha256_state configuration;
        crypto_hash_sha256_init(&configuration);
        bool wal = false, journal = false;
        for (const char* name : { "metadata.sqlite3", "metadata.sqlite3-wal", "metadata.sqlite3-journal", "metadata.sqlite3-shm", "config.toml", "instance.json" }) {
            auto file = detail::openPrivateFile(input, name, false);
            bool optional = std::string(name).find("sqlite3-") != std::string::npos;
            if (!file) {
                if (optional && file.error().code == ErrorCode::NotFound) {
                    std::string absent = std::string(name) + ":absent\n";
                    crypto_hash_sha256_update(&hash, reinterpret_cast<const unsigned char*>(absent.data()), absent.size());
                    continue;
                }
                return failure(c, MaintenanceStage::Capture, MaintenanceError::UnsafePath);
            }
            if (std::string(name) == "metadata.sqlite3-wal") wal = true;
            if (std::string(name) == "metadata.sqlite3-journal") journal = true;
            OwnedFd out;
            {
                out = OwnedFd(::openat(output.value().get(), std::string(name) == "metadata.sqlite3-shm" ? "metadata.sqlite3-shm.raw" : name, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600));
                if (out.get() < 0) return ioFailure(c, MaintenanceStage::Capture);
            }
            auto copied = hashCopy(c, file.value().get(), out.get());
            if (!copied) return failure(c, MaintenanceStage::Capture, MaintenanceError::CheckIncomplete);
            if (out.get() >= 0 && ::fsync(out.get())) return ioFailure(c, MaintenanceStage::Capture);
            auto again = detail::openPrivateFile(input, name, false);
            if (!again) return failure(c, MaintenanceStage::Capture, MaintenanceError::UnsafePath);
            auto reread = hashCopy(c, again.value().get());
            if (!reread || reread.value().sha256 != copied.value().sha256 || reread.value().size != copied.value().size) return failure(c, MaintenanceStage::Capture, MaintenanceError::CheckIncomplete);
            std::string record = std::string(name) + ":" + std::to_string(copied.value().size.value()) + ":" + copied.value().sha256.toString() + "\n";
            crypto_hash_sha256_update(&hash, reinterpret_cast<const unsigned char*>(record.data()), record.size());
            if (std::string(name) == "config.toml" || std::string(name) == "instance.json") crypto_hash_sha256_update(&configuration, reinterpret_cast<const unsigned char*>(record.data()), record.size());
        }
        if (wal && journal) return failure(c, MaintenanceStage::Capture, MaintenanceError::CheckIncomplete);
        auto after = inventoryFingerprint(c, input);
        if (!after || before.value() != after.value()) return failure(c, MaintenanceStage::Capture, MaintenanceError::CheckIncomplete);
        if (::fsync(output.value().get())) return ioFailure(c, MaintenanceStage::Capture);
        std::array<std::uint8_t, 32> bytes{};
        crypto_hash_sha256_final(&hash, bytes.data());
        std::array<std::uint8_t, 32> configHash{};
        crypto_hash_sha256_final(&configuration, configHash.data());
        return std::unique_ptr<InspectionCapture>(new Capture(path, { Sha256(bytes), before.value() }, Sha256(configHash), std::move(output).value()));
    }
    MaintenanceResult<std::unique_ptr<MaintenanceDestination>> acquireDestination(const MaintenanceContext& c, const std::filesystem::path& source, const std::filesystem::path& target) override {
        if (c.cancelled.load()) return failure(c, MaintenanceStage::AcquireLocks, MaintenanceError::Cancelled);
        if (!source.is_absolute() || !target.is_absolute() || target.filename().empty() || target.filename() == "." || target.filename() == ".." || overlap(source, target)) return failure(c, MaintenanceStage::AcquireLocks, MaintenanceError::UnsafePath);
        auto checkedSource = detail::openInstanceRoot(source);
        if (!checkedSource) return failure(c, MaintenanceStage::AcquireLocks, MaintenanceError::UnsafePath);
        auto parent = detail::openControlledDirectory(target.parent_path());
        if (!parent) return failure(c, MaintenanceStage::AcquireLocks, MaintenanceError::UnsafePath);
        auto name = target.filename().string();
        std::array<unsigned char, 32> hash{};
        crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(name.data()), name.size());
        auto lockName = ".anof-destination-" + Sha256(hash).toString() + ".lock";
        OwnedFd lock(::openat(parent.value().rootFd.get(), lockName.c_str(), O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (lock.get() < 0) {
            if (errno != EEXIST) return ioFailure(c, MaintenanceStage::AcquireLocks);
            auto existing = detail::openPrivateFile(parent.value().rootFd.get(), lockName.c_str(), true);
            if (!existing) return failure(c, MaintenanceStage::AcquireLocks, MaintenanceError::UnsafePath);
            lock = std::move(existing).value();
        }
        if (::flock(lock.get(), LOCK_EX | LOCK_NB)) return failure(c, MaintenanceStage::AcquireLocks, MaintenanceError::Busy);
        struct stat st{};
        if (::fstatat(parent.value().rootFd.get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) return failure(c, MaintenanceStage::AcquireLocks, MaintenanceError::AlreadyExists);
        if (errno != ENOENT) return ioFailure(c, MaintenanceStage::AcquireLocks);
        auto stageName = ".anof-staging-" + c.operationId.toString();
        if (::mkdirat(parent.value().rootFd.get(), stageName.c_str(), 0700)) return ioFailure(c, MaintenanceStage::AcquireLocks);
        auto stage = directory(parent.value().rootFd.get(), stageName);
        if (!stage) return failure(c, MaintenanceStage::AcquireLocks, MaintenanceError::UnsafePath);
        auto out = std::make_unique<Destination>();
        out->finalPath = parent.value().root / name;
        out->stagePath = parent.value().root / stageName;
        out->parent = std::move(parent.value().rootFd);
        out->lock = std::move(lock);
        out->stage = std::move(stage).value();
        return std::unique_ptr<MaintenanceDestination>(std::move(out));
    }
};
}  // namespace
Result<std::unique_ptr<OfflineInstanceAccess>> makeLinuxOfflineInstanceAccess() { return std::unique_ptr<OfflineInstanceAccess>(new Access); }
}  // namespace anof::infrastructure::system
