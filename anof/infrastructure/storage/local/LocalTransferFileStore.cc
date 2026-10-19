#include <anof/infrastructure/crypto/internal/SodiumSupport.h>
#include <anof/infrastructure/storage/local/internal/FileOperations.h>
#include <anof/infrastructure/system/internal/InstanceFiles.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sodium.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <new>

namespace anof::infrastructure::storage {
ssize_t detail::FileOperations::read(int fd, void* p, std::size_t n, off_t o) { return pread(fd, p, n, o); }
ssize_t detail::FileOperations::write(int fd, const void* p, std::size_t n, off_t o) { return pwrite(fd, p, n, o); }
int detail::FileOperations::sync(int fd, bool data) { return data ? fdatasync(fd) : fsync(fd); }
int detail::FileOperations::truncate(int fd, off_t n) { return ftruncate(fd, n); }
int detail::FileOperations::publish(int source, const char* name, int target, const char* object) { return static_cast<int>(syscall(SYS_renameat2, source, name, target, object, RENAME_NOREPLACE)); }
int detail::FileOperations::remove(int fd, const char* name) { return unlinkat(fd, name, 0); }
namespace {
using Fd = system::detail::OwnedFd;
using E = TransferFileError;
E error() {
    switch (errno) {
    case ENOENT:
        return E::NotFound;
    case EEXIST:
        return E::AlreadyExists;
    case ENOSPC:
    case EDQUOT:
        return E::NoSpace;
    case EAGAIN:
    case EINTR:
    case EMFILE:
    case ENFILE:
    case ENOMEM:
        return E::Transient;
    default:
        return E::Integrity;
    }
}
bool same(const struct stat& a, const struct stat& b) { return a.st_dev == b.st_dev && a.st_ino == b.st_ino; }
bool safe(const struct stat& s, bool directory) {
    return s.st_uid == geteuid() && (s.st_mode & 07777) == (directory ? 0700 : 0600) &&
           (directory ? S_ISDIR(s.st_mode) : S_ISREG(s.st_mode) && s.st_nlink == 1) && s.st_size >= 0;
}
bool syncFd(detail::FileOperations& ops, int fd, bool data = false) {
    int result;
    do {
        result = ops.sync(fd, data);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}
FileResult<Fd> directory(int parent, const std::string& name, dev_t device) {
    Fd fd(openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (fd.get() < 0) return error();
    struct stat a{}, b{};
    if (fstat(fd.get(), &a) || fstatat(parent, name.c_str(), &b, AT_SYMLINK_NOFOLLOW) ||
        !safe(a, true) || !same(a, b) || a.st_dev != device) return E::Integrity;
    return fd;
}
struct State {
    system::detail::InstanceDirectories dirs;
    Fd uploads, objects, quarantine;
    dev_t device;
    std::shared_ptr<detail::FileOperations> ops;
    bool valid() {
        auto current = system::detail::openInstanceDirectories(dirs.root);
        if (!current) return false;
        struct stat a{}, b{};
        if (fstat(dirs.rootFd.get(), &a) || fstat(current.value().rootFd.get(), &b) || !same(a, b)) return false;
        if (fstat(dirs.anofFd.get(), &a) || fstat(current.value().anofFd.get(), &b) || !same(a, b)) return false;
        const std::pair<const char*, int> entries[] = { { "uploads", uploads.get() }, { "objects", objects.get() }, { "quarantine", quarantine.get() } };
        for (auto entry : entries) {
            if (fstat(entry.second, &a) || fstatat(dirs.anofFd.get(), entry.first, &b, AT_SYMLINK_NOFOLLOW) ||
                !safe(a, true) || !same(a, b) || a.st_dev != device) return false;
        }
        return true;
    }
};
struct Path {
    Fd parent;
    std::string name;
    std::string shard;
};
struct Handle final : TransferFileHandle {
    std::shared_ptr<State> owner;
    Fd fd;
    Path path;
    bool writable;
    Handle(std::shared_ptr<State> s, Fd f, Path p, bool w) : owner(std::move(s)), fd(std::move(f)), path(std::move(p)), writable(w) {}
    bool valid() {
        if (!owner->valid()) return false;
        struct stat a{}, b{};
        if (!path.shard.empty()) {
            if (fstat(path.parent.get(), &a) || fstatat(owner->objects.get(), path.shard.c_str(), &b, AT_SYMLINK_NOFOLLOW) || !same(a, b) || !safe(a, true)) return false;
        }
        return !fstat(fd.get(), &a) && !fstatat(path.parent.get(), path.name.c_str(), &b, AT_SYMLINK_NOFOLLOW) && safe(a, false) && a.st_dev == owner->device && same(a, b);
    }
};
struct Hash final : TransferHashState {
    std::shared_ptr<State> owner;
    crypto_hash_sha256_state state{};
    explicit Hash(std::shared_ptr<State> s) : owner(std::move(s)) { crypto_hash_sha256_init(&state); }
    ~Hash() override { sodium_memzero(&state, sizeof(state)); }
};
class Store final : public TransferFileStore {
    std::shared_ptr<State> state_;
    FileResult<Path> path(const TransferFileKey& key, bool createShard = false) {
        if (!state_->valid()) return E::Integrity;
        if (const auto* t = std::get_if<TemporaryFile>(&key)) {
            Fd fd(fcntl(state_->uploads.get(), F_DUPFD_CLOEXEC, 0));
            if (fd.get() < 0) return error();
            return Path{ std::move(fd), t->id.toString() + ".part", {} };
        }
        auto name = std::get<ObjectFile>(key).id.toString();
        auto shard = name.substr(0, 2);
        if (createShard) {
            if (mkdirat(state_->objects.get(), shard.c_str(), 0700) && errno != EEXIST) return error();
            if (!syncFd(*state_->ops, state_->objects.get())) return E::UnknownEffect;
        }
        auto dir = directory(state_->objects.get(), shard, state_->device);
        if (auto* e = std::get_if<E>(&dir)) return *e;
        return Path{ std::move(std::get<Fd>(dir)), name, shard };
    }
    Handle* handle(TransferFileHandle& h, bool writable = false) {
        auto* value = dynamic_cast<Handle*>(&h);
        return value && value->owner == state_ && (!writable || value->writable) && value->valid() ? value : nullptr;
    }
    bool bounds(UploadOffset offset, std::size_t count) {
        return count <= kTransferBlockBytes && offset.value() <= kMaxFileBytes &&
               static_cast<std::uint64_t>(count) <= static_cast<std::uint64_t>(kMaxFileBytes - offset.value()) &&
               offset.value() <= std::numeric_limits<off_t>::max() - static_cast<std::int64_t>(count);
    }

public:
    explicit Store(std::shared_ptr<State> s) : state_(std::move(s)) {}
    FileResult<OwnedTransferFile> create(UploadId id) override {
        auto p = path(TemporaryFile{ id });
        if (auto* e = std::get_if<E>(&p)) return *e;
        auto value = std::move(std::get<Path>(p));
        Fd fd(openat(value.parent.get(), value.name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (fd.get() < 0) return error();
        auto h = std::make_unique<Handle>(state_, std::move(fd), std::move(value), true);
        if (!h->valid()) return E::Integrity;
        if (!syncFd(*state_->ops, h->fd.get(), true) || !syncFd(*state_->ops, h->path.parent.get())) return E::UnknownEffect;
        return OwnedTransferFile(std::move(h));
    }
    FileResult<OwnedTransferFile> open(const TransferFileKey& key, bool writable) override {
        if (writable && std::holds_alternative<ObjectFile>(key)) return E::Integrity;
        auto p = path(key);
        if (auto* e = std::get_if<E>(&p)) return *e;
        auto value = std::move(std::get<Path>(p));
        struct stat original{};
        if (fstatat(value.parent.get(), value.name.c_str(), &original, AT_SYMLINK_NOFOLLOW)) return error();
        if (!safe(original, false)) return E::Integrity;
        // O_NONBLOCK 防止被替换的 FIFO 阻塞 worker；随后检查真实类型。
        Fd fd(openat(value.parent.get(), value.name.c_str(), (writable ? O_RDWR : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (fd.get() < 0) return error();
        struct stat opened{};
        if (fstat(fd.get(), &opened) || !same(original, opened)) return E::Integrity;
        auto h = std::make_unique<Handle>(state_, std::move(fd), std::move(value), writable);
        if (!h->valid()) return E::Integrity;
        return OwnedTransferFile(std::move(h));
    }
    FileResult<TransferFileStat> stat(TransferFileHandle& h) override {
        auto* f = handle(h);
        struct stat s{};
        if (!f || fstat(f->fd.get(), &s) || !safe(s, false)) return E::Integrity;
        auto count = ByteCount::from(s.st_size);
        if (!count) return E::Integrity;
        return TransferFileStat{ count.value() };
    }
    FileStatus write(TransferFileHandle& h, UploadOffset offset, const TransferBlock& block) override {
        if (!bounds(offset, block.size)) return E::Integrity;
        auto* f = handle(h, true);
        if (!f) return E::Integrity;
        std::size_t done = 0;
        while (done < block.size) {
            auto n = state_->ops->write(f->fd.get(), block.bytes.data() + done, block.size - done, static_cast<off_t>(offset.value()) + static_cast<off_t>(done));
            if (n < 0) {
                if (errno == EINTR) continue;
                return error();
            }
            if (!n) return E::Integrity;
            done += static_cast<std::size_t>(n);
        }
        return f->valid() ? FileStatus(std::monostate{}) : FileStatus(E::Integrity);
    }
    FileStatus read(TransferFileHandle& h, UploadOffset offset, std::size_t count, TransferBlock& block) override {
        block.size = 0;
        if (!bounds(offset, count)) return E::Integrity;
        auto* f = handle(h);
        if (!f) return E::Integrity;
        std::size_t done = 0;
        while (done < count) {
            auto n = state_->ops->read(f->fd.get(), block.bytes.data() + done, count - done, static_cast<off_t>(offset.value()) + static_cast<off_t>(done));
            if (n < 0) {
                if (errno == EINTR) continue;
                return error();
            }
            if (!n) return E::Integrity;
            done += static_cast<std::size_t>(n);
        }
        if (!f->valid()) return E::Integrity;
        block.size = count;
        return std::monostate{};
    }
    FileStatus syncData(TransferFileHandle& h) override {
        auto* f = handle(h);
        if (!f) return E::Integrity;
        if (!syncFd(*state_->ops, f->fd.get(), true)) return E::UnknownEffect;
        return f->valid() ? FileStatus(std::monostate{}) : FileStatus(E::Integrity);
    }
    FileStatus truncateAndSync(TransferFileHandle& h, UploadOffset offset) override {
        auto* f = handle(h, true);
        if (!f || !bounds(offset, 0)) return E::Integrity;
        struct stat s{};
        if (fstat(f->fd.get(), &s) || offset.value() > s.st_size) return E::Integrity;
        int result;
        do {
            result = state_->ops->truncate(f->fd.get(), static_cast<off_t>(offset.value()));
        } while (result && errno == EINTR);
        if (result || !syncFd(*state_->ops, f->fd.get(), true)) return E::UnknownEffect;
        return f->valid() ? FileStatus(std::monostate{}) : FileStatus(E::Integrity);
    }
    FileStatus publish(UploadId upload, BlobId blob) override {
        auto source = open(TemporaryFile{ upload }, false);
        if (auto* e = std::get_if<E>(&source)) return *e;
        auto& h = *std::get<OwnedTransferFile>(source);
        auto synced = syncData(h);
        if (auto* e = std::get_if<E>(&synced)) return *e;
        auto target = path(ObjectFile{ blob }, true);
        if (auto* e = std::get_if<E>(&target)) return *e;
        auto& to = std::get<Path>(target);
        auto* f = handle(h);
        if (!f) return E::Integrity;
        struct stat a{}, b{};
        if (fstat(to.parent.get(), &a) || fstatat(state_->objects.get(), to.shard.c_str(), &b, AT_SYMLINK_NOFOLLOW) || !same(a, b) || !safe(a, true)) return E::Integrity;
        if (state_->ops->publish(f->path.parent.get(), f->path.name.c_str(), to.parent.get(), to.name.c_str())) {
            if (errno == EEXIST) return E::AlreadyExists;
            if (errno == EXDEV) return E::Integrity;
            return E::UnknownEffect;
        }
        f->path = std::move(to);
        if (!f->valid() || !syncFd(*state_->ops, f->path.parent.get()) || !syncFd(*state_->ops, state_->uploads.get()) || !syncFd(*state_->ops, state_->objects.get())) return E::UnknownEffect;
        return std::monostate{};
    }
    FileStatus syncPublication(UploadId upload, BlobId blob) override {
        auto source = path(TemporaryFile{ upload });
        if (auto* e = std::get_if<E>(&source)) return *e;
        auto& from = std::get<Path>(source);
        auto absent = [&]() {
            struct stat status{};
            return fstatat(from.parent.get(), from.name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
        };
        if (!absent()) return E::Integrity;
        auto object = open(ObjectFile{ blob }, false);
        if (auto* e = std::get_if<E>(&object)) return *e == E::NotFound ? E::Integrity : *e;
        auto* target = handle(*std::get<OwnedTransferFile>(object));
        if (!target || !absent()) return E::Integrity;
        // 内容与持久意图由调用方先验证；这里只补齐已移动对象的耐久同步。
        if (!syncFd(*state_->ops, target->fd.get(), true) ||
            !syncFd(*state_->ops, target->path.parent.get()) ||
            !syncFd(*state_->ops, state_->uploads.get()) ||
            !syncFd(*state_->ops, state_->objects.get())) return E::UnknownEffect;
        if (!target->valid() || !absent()) return E::UnknownEffect;
        return std::monostate{};
    }
    FileStatus removeAndSync(const TransferFileKey& key) override {
        auto p = path(key);
        if (auto* e = std::get_if<E>(&p)) {
            if (*e != E::NotFound || !std::holds_alternative<ObjectFile>(key)) return *e;
            return syncFd(*state_->ops, state_->objects.get()) ? FileStatus(std::monostate{}) : FileStatus(E::UnknownEffect);
        }
        auto& value = std::get<Path>(p);
        auto file = open(key, false);
        if (auto* e = std::get_if<E>(&file)) {
            if (*e != E::NotFound) return *e;
        } else {
            auto* f = handle(*std::get<OwnedTransferFile>(file));
            if (!f) return E::Integrity;
            if (state_->ops->remove(f->path.parent.get(), f->path.name.c_str())) return E::UnknownEffect;
        }
        if (!value.shard.empty()) {
            struct stat a{}, b{};
            if (fstat(value.parent.get(), &a) || fstatat(state_->objects.get(), value.shard.c_str(), &b, AT_SYMLINK_NOFOLLOW) || !same(a, b)) return E::UnknownEffect;
        }
        if (!syncFd(*state_->ops, value.parent.get()) || !state_->valid()) return E::UnknownEffect;
        return std::monostate{};
    }
    FileResult<OwnedTransferHash> beginHash() override { return OwnedTransferHash(new Hash(state_)); }
    FileStatus updateHash(TransferHashState& hash, const TransferBlock& block) override {
        auto* h = dynamic_cast<Hash*>(&hash);
        if (!h || h->owner != state_ || block.size > block.bytes.size()) return E::Integrity;
        if (crypto_hash_sha256_update(&h->state, block.bytes.data(), block.size)) return E::Integrity;
        return std::monostate{};
    }
    FileResult<Sha256> finishHash(OwnedTransferHash hash) override {
        auto* h = dynamic_cast<Hash*>(hash.get());
        if (!h || h->owner != state_) return E::Integrity;
        std::array<std::uint8_t, 32> digest{};
        if (crypto_hash_sha256_final(&h->state, digest.data())) return E::Integrity;
        return Sha256(digest);
    }
    FileResult<DiskCapacity> capacity() override {
        if (!state_->valid()) return E::Integrity;
        struct statvfs s{};
        if (fstatvfs(state_->uploads.get(), &s)) return error();
        const auto max = static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max());
        if (!s.f_frsize || s.f_bavail > max / s.f_frsize || s.f_blocks > max / s.f_frsize) return E::Integrity;
        return DiskCapacity{ ByteCount::from(static_cast<std::int64_t>(s.f_bavail * s.f_frsize)).value(), ByteCount::from(static_cast<std::int64_t>(s.f_blocks * s.f_frsize)).value() };
    }
    FileResult<Page<InventoryEntry, InventoryCursor>> inventory(InventoryArea area, PageSize size, std::optional<InventoryCursor> after) override;
};
// 每次扫描只保留最小的 size+1 项；不把目录完整物化。枚举期间目录必须由启动 lease 独占。
FileResult<Page<InventoryEntry, InventoryCursor>> Store::inventory(InventoryArea area, PageSize size, std::optional<InventoryCursor> after) {
    if (!state_->valid()) return E::Integrity;
    int base;
    if (area == InventoryArea::Uploads)
        base = state_->uploads.get();
    else if (area == InventoryArea::Objects)
        base = state_->objects.get();
    else if (area == InventoryArea::Quarantine)
        base = state_->quarantine.get();
    else
        return E::Integrity;
    const std::string prefix = std::to_string(static_cast<int>(area)) + ":";
    if (after && (after->opaque.size() > 512 || after->opaque.compare(0, prefix.size(), prefix) != 0)) return E::Integrity;
    const auto seek = after ? after->opaque.substr(prefix.size()) : std::string{};
    std::vector<InventoryEntry> entries;
    auto visit = [&](int parent, const std::string& name, const std::string& relative) -> FileStatus {
        struct stat s{};
        if (fstatat(parent, name.c_str(), &s, AT_SYMLINK_NOFOLLOW) || !safe(s, false) || s.st_dev != state_->device) return E::Integrity;
        if (relative <= seek) return std::monostate{};
        std::optional<TransferFileKey> key;
        if (area == InventoryArea::Uploads && name.size() == 41 && name.substr(36) == ".part") {
            auto id = UploadId::parse(name.substr(0, 36));
            if (id && id.value().toString() == name.substr(0, 36)) key = TemporaryFile{ id.value() };
        } else if (area == InventoryArea::Objects && relative.size() == 39) {
            auto id = BlobId::parse(name);
            if (id && id.value().toString() == name && relative.substr(0, 2) == name.substr(0, 2)) key = ObjectFile{ id.value() };
        }
        entries.push_back({ relative, key, ByteCount::from(s.st_size).value() });
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.relativeName < b.relativeName; });
        if (entries.size() > static_cast<std::size_t>(size.value() + 1)) entries.pop_back();
        return std::monostate{};
    };
    auto scan = [&](int parent, auto visitor) -> FileStatus {
        Fd fd(openat(parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (fd.get() < 0) return error();
        // fdopendir 接管自己的 dup；独立 open 避免共享目录偏移。
        int copied = fcntl(fd.get(), F_DUPFD_CLOEXEC, 0);
        if (copied < 0) return error();
        DIR* raw = fdopendir(copied);
        if (!raw) {
            close(copied);
            return error();
        }
        auto closeDirectory = [](DIR* p) { (void)closedir(p); };
        std::unique_ptr<DIR, decltype(closeDirectory)> stream(raw, closeDirectory);
        for (;;) {
            errno = 0;
            auto* entry = readdir(raw);
            if (!entry) return errno ? FileStatus(error()) : FileStatus(std::monostate{});
            std::string name(entry->d_name);
            if (name == "." || name == "..") continue;
            auto result = visitor(name);
            if (std::holds_alternative<E>(result)) return result;
        }
    };
    auto result = scan(base, [&](const std::string& name) -> FileStatus {
        if (area == InventoryArea::Objects && name.size() == 2 && name.find_first_not_of("0123456789abcdef") == std::string::npos) {
            auto shard = directory(base, name, state_->device);
            if (auto* e = std::get_if<E>(&shard)) return *e;
            int fd = std::get<Fd>(shard).get();
            return scan(fd, [&](const std::string& child) { return visit(fd, child, name + "/" + child); });
        }
        return visit(base, name, name);
    });
    if (auto* e = std::get_if<E>(&result)) return *e;
    if (!state_->valid()) return E::Integrity;
    Page<InventoryEntry, InventoryCursor> page;
    if (entries.size() > static_cast<std::size_t>(size.value())) {
        entries.pop_back();
        page.next = InventoryCursor{ prefix + entries.back().relativeName };
    }
    page.items = std::move(entries);
    return page;
}
}  // namespace
FileResult<std::unique_ptr<TransferFileStore>> detail::openLocalTransferFileStore(const std::filesystem::path& root, std::shared_ptr<FileOperations> ops) {
    if (!ops) return E::Integrity;
    auto initialized = crypto::detail::initializeSodium();
    if (!initialized) return E::Transient;
    auto dirs = system::detail::openInstanceDirectories(root);
    if (!dirs) return E::Integrity;
    struct stat s{};
    if (fstat(dirs.value().anofFd.get(), &s)) return E::Integrity;
    auto uploads = directory(dirs.value().anofFd.get(), "uploads", s.st_dev);
    auto objects = directory(dirs.value().anofFd.get(), "objects", s.st_dev);
    auto quarantine = directory(dirs.value().anofFd.get(), "quarantine", s.st_dev);
    if (std::holds_alternative<E>(uploads) || std::holds_alternative<E>(objects) || std::holds_alternative<E>(quarantine)) return E::Integrity;
    try {
        auto state = std::make_shared<State>(State{ std::move(dirs).value(), std::move(std::get<Fd>(uploads)), std::move(std::get<Fd>(objects)), std::move(std::get<Fd>(quarantine)), s.st_dev, std::move(ops) });
        return std::unique_ptr<TransferFileStore>(new Store(std::move(state)));
    } catch (const std::bad_alloc&) {
        return E::Transient;
    }
}
FileResult<std::unique_ptr<TransferFileStore>> openLocalTransferFileStore(const std::filesystem::path& root) {
    return detail::openLocalTransferFileStore(root, std::make_shared<detail::FileOperations>());
}
}  // namespace anof::infrastructure::storage
