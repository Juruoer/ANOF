#include <anof/client/internal/TransferFiles.h>
#include <anof/domain/transfer/TransferValues.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sodium.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
namespace anof::client::transfer_internal {
[[noreturn]] void fail(ErrorKind kind, std::string code) { throw Failure{ failure(kind, std::move(code)) }; }
void check(bool ok, std::string code) {
    if (!ok) fail(ErrorKind::LocalState, std::move(code));
}
Fd::~Fd() {
    if (value >= 0) ::close(value);
}
Fd::Fd(Fd&& o) noexcept : value(std::exchange(o.value, -1)) {}
Fd& Fd::operator=(Fd&& o) noexcept {
    if (this != &o) {
        if (value >= 0) ::close(value);
        value = std::exchange(o.value, -1);
    }
    return *this;
}
bool absolute(const std::string& p) {
    if (p.size() < 2 || p.front() != '/' || p.back() == '/' || p.size() > 4096 || p.find('\0') != std::string::npos)
        return false;
    std::size_t begin = 1;
    while (begin < p.size()) {
        auto end = p.find('/', begin);
        if (end == std::string::npos) end = p.size();
        auto part = p.substr(begin, end - begin);
        if (part.empty() || part == "." || part == "..") return false;
        begin = end + 1;
    }
    return true;
}
struct stat status(int fd) {
    struct stat s{};
    check(fd >= 0 && fstat(fd, &s) == 0);
    return s;
}
void safeFile(int fd) {
    auto s = status(fd);
    check(S_ISREG(s.st_mode) && s.st_uid == geteuid() && s.st_nlink == 1 && (s.st_mode & 07777) == 0600);
}
Fd directory(const std::string& p, bool privateMode) {
    if (p != "/" && !absolute(p)) fail(ErrorKind::InvalidArgument);
    Fd fd(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    check(fd.value >= 0);
    std::size_t begin = 1;
    while (begin < p.size()) {
        auto end = p.find('/', begin);
        if (end == std::string::npos) end = p.size();
        auto part = p.substr(begin, end - begin);
        Fd next(openat(fd.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        check(next.value >= 0);
        fd = std::move(next);
        begin = end + 1;
    }
    auto s = status(fd.value);
    check(s.st_uid == geteuid() && (s.st_mode & 0022) == 0);
    if (privateMode) check((s.st_mode & 07777) == 0700);
    return fd;
}
Json identity(int fd) {
    auto s = status(fd);
    check(S_ISREG(s.st_mode) && s.st_size >= 0 && s.st_size <= kMaxFileBytes);
    return { { "dev", std::to_string(s.st_dev) },
             { "ino", std::to_string(s.st_ino) },
             { "size", std::to_string(s.st_size) },
             { "mtime", std::to_string(s.st_mtim.tv_sec) + ":" + std::to_string(s.st_mtim.tv_nsec) },
             { "ctime", std::to_string(s.st_ctim.tv_sec) + ":" + std::to_string(s.st_ctim.tv_nsec) } };
}
void writeAll(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        auto n = write(fd, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR) continue;
        check(n > 0);
        bytes.remove_prefix(static_cast<std::size_t>(n));
    }
}
std::string hash(int fd, const std::atomic_bool* cancel) {
    check(sodium_init() >= 0);
    crypto_hash_sha256_state s;
    crypto_hash_sha256_init(&s);
    std::array<unsigned char, 65536> bytes{};
    off_t offset = 0;
    for (;;) {
        if (cancel && cancel->load()) fail(ErrorKind::Interrupted);
        auto n = pread(fd, bytes.data(), bytes.size(), offset);
        if (n < 0 && errno == EINTR) continue;
        check(n >= 0);
        if (!n) break;
        check(offset <= kMaxFileBytes - n);
        crypto_hash_sha256_update(&s, bytes.data(), static_cast<unsigned long long>(n));
        offset += n;
    }
    std::array<std::uint8_t, 32> out{};
    crypto_hash_sha256_final(&s, out.data());
    return Sha256(out).toString();
}
std::string uuid() {
    check(sodium_init() >= 0);
    unsigned char bytes[16];
    randombytes_buf(bytes, sizeof bytes);
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
        result += hex[bytes[i] >> 4];
        result += hex[bytes[i] & 15];
    }
    return result;
}
Record::Record(const std::string& root, const Profile& profile, std::optional<std::string> supplied) {
    check(profile.credential.has_value());
    auto base = directory(root, true);
    if (mkdirat(base.value, "transfers", 0700) != 0) check(errno == EEXIST);
    check(fsync(base.value) == 0);
    dir_ = directory(root + "/transfers", true);
    Fd catalog(openat(dir_.value, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
    safeFile(catalog.value);
    check(flock(catalog.value, LOCK_EX | LOCK_NB) == 0, "TRANSFER_BUSY");
    if (supplied) {
        auto id = OperationId::parse(*supplied);
        if (!id || id.value().toString() != *supplied) fail(ErrorKind::InvalidArgument);
        id_ = *supplied;
    } else {
        Fd duplicate(openat(dir_.value, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        check(duplicate.value >= 0);
        struct Close {
            void operator()(DIR* dir) const { closedir(dir); }
        };
        std::unique_ptr<DIR, Close> entries(fdopendir(duplicate.value));
        check(bool(entries));
        duplicate.value = -1;
        unsigned count = 0;
        errno = 0;
        while (auto* item = readdir(entries.get())) {
            std::string name = item->d_name;
            if (name.size() > 5 && name.substr(name.size() - 5) == ".json") ++count;
            errno = 0;
        }
        check(errno == 0);
        check(count < 1024, "TRANSFER_LIMIT_REACHED");
        id_ = uuid();
    }
    lock_ =
        Fd(openat(dir_.value, (id_ + ".lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
    safeFile(lock_.value);
    check(flock(lock_.value, LOCK_EX | LOCK_NB) == 0, "TRANSFER_BUSY");
    if (supplied) {
        Fd fd(openat(dir_.value, (id_ + ".json").c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        safeFile(fd.value);
        auto s = status(fd.value);
        check(s.st_size > 0 && s.st_size <= 16384);
        std::string bytes(static_cast<std::size_t>(s.st_size), '\0');
        std::size_t done = 0;
        while (done < bytes.size()) {
            auto n = read(fd.value, bytes.data() + done, bytes.size() - done);
            if (n < 0 && errno == EINTR) continue;
            check(n > 0);
            done += static_cast<std::size_t>(n);
        }
        internal::Document doc;
        doc.parse(bytes);
        data = doc.root;
        check(data.at("version") == 1 && data.at("transfer_id") == id_ && data.at("profile") == profile.name &&
                  data.at("origin") == profile.server.value() &&
                  data.at("user_id") == profile.credential->userId.toString(),
              "TRANSFER_BINDING_MISMATCH");
        created_ = true;
    } else {
        data = { { "version", 1 },
                 { "transfer_id", id_ },
                 { "profile", profile.name },
                 { "origin", profile.server.value() },
                 { "user_id", profile.credential->userId.toString() },
                 { "instance_id", nullptr },
                 { "phase", "NEW" } };
        save();
    }
}
void Record::save() {
    auto bytes = data.dump();
    check(bytes.size() <= 16384);
    const auto tmp = ".tmp-" + uuid();
    Fd fd(openat(dir_.value, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    safeFile(fd.value);
    struct Cleanup {
        int dir;
        std::string path;
        ~Cleanup() { unlinkat(dir, path.c_str(), 0); }
    } cleanup{ dir_.value, tmp };
    writeAll(fd.value, bytes);
    check(fsync(fd.value) == 0);
    const auto target = id_ + ".json";
    if (created_) {
        Fd old(openat(dir_.value, target.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        safeFile(old.value);
        check(renameat(dir_.value, tmp.c_str(), dir_.value, target.c_str()) == 0);
    } else
        check(syscall(SYS_renameat2, dir_.value, tmp.c_str(), dir_.value, target.c_str(), RENAME_NOREPLACE) == 0);
    created_ = true;
    if (fsync(dir_.value) != 0) {
        auto error = failure(ErrorKind::LocalState);
        error.local = LocalState::Unknown;
        throw Failure{ error };
    }
}
}  // namespace anof::client::transfer_internal
