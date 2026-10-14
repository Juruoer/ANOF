#include <anof/client/ProfileStore.h>
#include <anof/client/internal/Json.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace anof::client {
namespace {
using namespace internal;
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    Fd& operator=(Fd&& other) noexcept { if (this != &other) { if (value >= 0) ::close(value); value = std::exchange(other.value, -1); } return *this; }
    Fd(const Fd&) = delete;
};
struct LocalFailure { ClientError error; };
[[noreturn]] void fail(std::string code = "LOCAL_STATE_ERROR", LocalState state = LocalState::Unchanged) { auto e = failure(ErrorKind::LocalState, std::move(code)); e.local = state; throw LocalFailure{std::move(e)}; }
void checked(bool value) { if (!value) fail(); }
void safeFile(int fd) {
    struct stat st{};
    checked(fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 07777) == 0600 && st.st_nlink == 1);
}
void safeDirectory(int fd) { struct stat st{}; checked(fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 07777) == 0700); }
bool validPath(std::string_view path) {
    if (path.size() < 2 || path.front() != '/' || path.size() > 4096 || path.find('\0') != std::string_view::npos) return false;
    std::size_t start = 1;
    while (start < path.size()) { auto end = path.find('/', start); if (end == std::string_view::npos) end = path.size(); const auto part = path.substr(start, end - start); if (part.empty() || part == "." || part == "..") return false; start = end + 1; }
    return path.back() != '/';
}
Fd directory(std::string_view path, bool create) {
    if (!validPath(path)) throw LocalFailure{failure(ErrorKind::InvalidArgument)};
    Fd current(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)); checked(current.value >= 0);
    std::size_t start = 1;
    while (start < path.size()) {
        auto end = path.find('/', start); if (end == std::string_view::npos) end = path.size();
        const std::string part(path.substr(start, end - start));
        int next = openat(current.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            checked(mkdirat(current.value, part.c_str(), 0700) == 0 || errno == EEXIST);
            checked(fsync(current.value) == 0);
            next = openat(current.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0 && errno == ENOENT && !create) return Fd{};
        checked(next >= 0); current = Fd(next); start = end + 1;
    }
    safeDirectory(current.value);
    int profiles = openat(current.value, "profiles", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (profiles < 0 && errno == ENOENT && create) {
        checked(mkdirat(current.value, "profiles", 0700) == 0 || errno == EEXIST);
        checked(fsync(current.value) == 0);
        profiles = openat(current.value, "profiles", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (profiles < 0 && errno == ENOENT && !create) return Fd{};
    Fd result(profiles); checked(profiles >= 0); safeDirectory(profiles); return result;
}
Fd lockFile(int dir, const std::string& name) {
    Fd fd(openat(dir, name.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
    safeFile(fd.value);
    if (flock(fd.value, LOCK_EX | LOCK_NB) != 0) fail(errno == EWOULDBLOCK ? "PROFILE_BUSY" : "LOCAL_STATE_ERROR");
    return fd;
}
std::vector<std::string> names(int dir) {
    Fd copy(openat(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)); checked(copy.value >= 0);
    struct CloseDirectory { void operator()(DIR* d) const noexcept { closedir(d); } };
    std::unique_ptr<DIR, CloseDirectory> stream(fdopendir(copy.value)); checked(static_cast<bool>(stream)); copy.value = -1;
    std::vector<std::string> result;
    errno = 0;
    while (auto* entry = readdir(stream.get())) {
        std::string name(entry->d_name);
        if (name.size() > 5 && name.substr(name.size() - 5) == ".json") {
            name.resize(name.size() - 5);
            if (ProfileStore::validName(name)) { result.push_back(std::move(name)); if (result.size() > 100) fail("PROFILE_LIMIT_REACHED"); }
        }
        errno = 0;
    }
    checked(errno == 0); std::sort(result.begin(), result.end()); return result;
}
Profile readProfile(int dir, const std::string& name) try {
    Fd fd(openat(dir, (name + ".json").c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (fd.value < 0 && errno == ENOENT) fail("PROFILE_NOT_FOUND");
    safeFile(fd.value);
    struct stat st{}; checked(fstat(fd.value, &st) == 0 && st.st_size >= 0 && st.st_size <= 16384);
    SecureBuffer input(16385);
    for (;;) {
        char bytes[4096]; const auto count = read(fd.value, bytes, sizeof bytes);
        if (count < 0 && errno == EINTR) continue;
        checked(count >= 0);
        const bool fits = input.append({bytes, static_cast<std::size_t>(count)});
        volatile char* wipeBytes = bytes; for (std::size_t i = 0; i < sizeof bytes; ++i) wipeBytes[i] = 0;
        checked(fits && input.view().size() <= 16384);
        if (count == 0) break;
    }
    Document doc; doc.parse(input.view()); const auto& j = doc.root;
    exactKeys(j, {"version", "server", "development_http", "credential"}); require(integer(j.at("version"), 1, 1) == 1);
    auto origin = Origin::parse(string(j, "server"), boolean(j, "development_http"));
    require(origin && origin.value().value() == string(j, "server"));
    Profile profile{name, std::move(origin).value(), {}};
    const auto& c = j.at("credential");
    if (!c.is_null()) {
        exactKeys(c, {"access_token", "expires_at", "user_id", "login"});
        auto token = SecretToken::session(string(c, "access_token")); require(static_cast<bool>(token));
        profile.credential.emplace(StoredCredential{std::move(token).value(), time(field(c, "expires_at")), id<UserId>(field(c, "user_id")), login(field(c, "login"))});
    }
    return profile;
} catch (LocalFailure& e) {
    e.error.profileName = name; throw;
} catch (...) {
    auto error = failure(ErrorKind::LocalState, "PROFILE_INVALID"); error.profileName = name; throw LocalFailure{std::move(error)};
}
ProfileSummary summarize(const Profile& p) {
    ProfileSummary result{p.name, p.server.value(), p.server.developmentHttp(), {}};
    if (p.credential) result.session.emplace(SessionSummary{p.credential->expiresAt, p.credential->userId, p.credential->login});
    return result;
}
void save(int dir, const Profile& p, const StoredCredential* credential, bool create, const SaveHook& hook) {
    SecureBuffer output(16384);
    append(output, "{\"version\":1,\"server\":"); quoted(output, p.server.value());
    append(output, p.server.developmentHttp() ? ",\"development_http\":true,\"credential\":" : ",\"development_http\":false,\"credential\":");
    if (!credential) append(output, "null");
    else {
        require(SecretToken::session(credential->accessToken.view()) && validTimestamp(credential->expiresAt));
        append(output, "{\"access_token\":"); quoted(output, credential->accessToken.view());
        append(output, ",\"expires_at\":"); quoted(output, credential->expiresAt);
        append(output, ",\"user_id\":"); quoted(output, credential->userId.toString());
        append(output, ",\"login\":"); quoted(output, credential->login.value()); append(output, "}");
    }
    append(output, "}\n");
    static std::atomic<unsigned long> serial{0};
    std::string temp;
    Fd fd;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        temp = ".tmp-" + std::to_string(getpid()) + "-" + std::to_string(serial.fetch_add(1));
        fd = Fd(openat(dir, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (fd.value >= 0 || errno != EEXIST) break;
    }
    checked(fd.value >= 0);
    struct Cleanup { int dir; const std::string& name; bool active = true; ~Cleanup() { if (active) unlinkat(dir, name.c_str(), 0); } } cleanup{dir, temp};
    safeFile(fd.value);
    std::size_t written = 0;
    while (written < output.view().size()) {
        const auto count = write(fd.value, output.view().data() + written, output.view().size() - written);
        if (count < 0 && errno == EINTR) continue;
        checked(count > 0); written += static_cast<std::size_t>(count);
    }
    checked(fsync(fd.value) == 0);
    if (hook && !hook(SavePoint::BeforePublish)) fail();
    const auto target = p.name + ".json";
    if (create) {
        if (syscall(SYS_renameat2, dir, temp.c_str(), dir, target.c_str(), RENAME_NOREPLACE) != 0)
            fail(errno == EEXIST ? "PROFILE_EXISTS" : "LOCAL_STATE_ERROR");
    } else checked(renameat(dir, temp.c_str(), dir, target.c_str()) == 0);
    cleanup.active = false;
    try { if ((hook && !hook(SavePoint::AfterPublish)) || fsync(dir) != 0) fail("LOCAL_STATE_ERROR", LocalState::Unknown); }
    catch (...) { fail("LOCAL_STATE_ERROR", LocalState::Unknown); }
}
template<class T, class F> ClientResult<T> guarded(F&& work) {
    try { return work(); } catch (const LocalFailure& e) { return e.error; } catch (...) { return failure(ErrorKind::LocalState); }
}
}  // namespace
struct LockedProfile::State { Fd directory, lock; Profile profile; SaveHook hook; bool usable = true; };
LockedProfile::LockedProfile(std::unique_ptr<State> state) : state_(std::move(state)) {}
LockedProfile::~LockedProfile() = default;
LockedProfile::LockedProfile(LockedProfile&&) noexcept = default;
LockedProfile& LockedProfile::operator=(LockedProfile&&) noexcept = default;
const Profile& LockedProfile::profile() const noexcept { return state_->profile; }
ProfileSummary LockedProfile::summary() const { return summarize(state_->profile); }
bool LockedProfile::usable() const noexcept { return state_ && state_->usable; }
ClientResult<Done> LockedProfile::saveCredential(StoredCredential credential) {
    auto result = guarded<Done>([&]() -> Done { checked(state_->usable); save(state_->directory.value, state_->profile, &credential, false, state_->hook); state_->profile.credential = std::move(credential); return {}; });
    if (!result && result.error().local == LocalState::Unknown) state_->usable = false;
    return result;
}
ClientResult<Done> LockedProfile::clearCredential() {
    auto result = guarded<Done>([&]() -> Done { checked(state_->usable); if (state_->profile.credential) { save(state_->directory.value, state_->profile, nullptr, false, state_->hook); state_->profile.credential.reset(); } return {}; });
    if (!result && result.error().local == LocalState::Unknown) state_->usable = false;
    return result;
}
bool ProfileStore::validName(std::string_view name) noexcept {
    if (name.empty() || name.size() > 32 || name.front() < 'a' || name.front() > 'z') return false;
    for (char c : name) if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_' && c != '-') return false;
    return true;
}
ClientResult<std::string> ProfileStore::resolveDirectory(std::optional<std::string> explicitPath) {
    std::string path;
    if (explicitPath) path = *explicitPath;
    else if (const auto* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) { path = xdg; if (path.back() == '/') path.pop_back(); path += "/anof"; }
    else { const auto* home = std::getenv("HOME"); if (!home || !*home) return failure(ErrorKind::LocalState); path = home; if (path.back() == '/') path.pop_back(); path += "/.config/anof"; }
    if (!validPath(path)) return failure(ErrorKind::InvalidArgument);
    return path;
}
ProfileStore::ProfileStore(std::string directory, SaveHook hook) : directory_(std::move(directory)), hook_(std::move(hook)) {}
ClientResult<Done> ProfileStore::add(std::string name, const Origin& server) {
    if (!validName(name)) return failure(ErrorKind::InvalidArgument);
    return guarded<Done>([&]() -> Done {
        auto dir = directory(directory_, true); auto directoryLock = lockFile(dir.value, ".directory.lock");
        auto profileLock = lockFile(dir.value, "." + name + ".lock");
        if (names(dir.value).size() >= 100) fail("PROFILE_LIMIT_REACHED");
        save(dir.value, Profile{std::move(name), server, {}}, nullptr, true, hook_); return {};
    });
}
ClientResult<LockedProfile> ProfileStore::lock(std::string name) const {
    if (!validName(name)) return failure(ErrorKind::InvalidArgument);
    return guarded<LockedProfile>([&]() -> LockedProfile {
        auto dir = directory(directory_, false); if (dir.value < 0) fail("PROFILE_NOT_FOUND");
        auto held = lockFile(dir.value, "." + name + ".lock"); auto profile = readProfile(dir.value, name);
        return LockedProfile(std::make_unique<LockedProfile::State>(LockedProfile::State{std::move(dir), std::move(held), std::move(profile), hook_, true}));
    });
}
ClientResult<ProfileSummary> ProfileStore::show(std::string name) const { auto p = lock(std::move(name)); if (!p) return p.error(); return p.value().summary(); }
ClientResult<std::vector<ProfileSummary>> ProfileStore::list() const {
    return guarded<std::vector<ProfileSummary>>([&]() {
        std::vector<ProfileSummary> result; auto dir = directory(directory_, false); if (dir.value < 0) return result;
        auto directoryLock = lockFile(dir.value, ".directory.lock");
        for (const auto& name : names(dir.value)) { auto held = lockFile(dir.value, "." + name + ".lock"); result.push_back(summarize(readProfile(dir.value, name))); }
        return result;
    });
}
ClientResult<Done> ProfileStore::remove(std::string name) {
    if (!validName(name)) return failure(ErrorKind::InvalidArgument);
    return guarded<Done>([&]() -> Done {
        auto dir = directory(directory_, false); if (dir.value < 0) fail("PROFILE_NOT_FOUND");
        auto directoryLock = lockFile(dir.value, ".directory.lock"); auto held = lockFile(dir.value, "." + name + ".lock");
        // 损坏的内容仍可显式删除，但拒绝替换为不安全的文件对象。
        Fd fd(openat(dir.value, (name + ".json").c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)); safeFile(fd.value);
        checked(unlinkat(dir.value, (name + ".json").c_str(), 0) == 0);
        if (fsync(dir.value) != 0) fail("LOCAL_STATE_ERROR", LocalState::Unknown);
        return {};
    });
}
}  // namespace anof::client
