#define BOOST_TEST_MODULE InstanceLock
#include <anof/infrastructure/system/LinuxInstanceLock.h>
#include <anof/infrastructure/system/internal/InstanceFiles.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <boost/test/unit_test.hpp>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace anof;
using namespace anof::infrastructure::system;
namespace fs = std::filesystem;

namespace {
struct Fixture {
    fs::path base;
    fs::path root;
    std::unique_ptr<InstanceLockProvider> provider;
    Fixture() {
        BOOST_REQUIRE_MESSAGE(::geteuid() != 0, "Run filesystem tests as an unprivileged user");
        char pattern[] = "/tmp/anof-instance-lock-XXXXXX";
        char* directory = ::mkdtemp(pattern);
        BOOST_REQUIRE(directory);
        base = directory;
        root = base / "instance";
        BOOST_REQUIRE(::mkdir(root.c_str(), 0700) == 0);
        BOOST_REQUIRE(::mkdir((root / ".anof").c_str(), 0700) == 0);
        auto result = makeLinuxInstanceLockProvider();
        BOOST_REQUIRE(result);
        provider = std::move(result).value();
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(base, ignored);
    }
    fs::path lock() const { return root / ".anof/instance.lock"; }
    void file(const fs::path& path, mode_t mode = 0600) {
        std::ofstream(path) << "preserve";
        BOOST_REQUIRE(::chmod(path.c_str(), mode) == 0);
    }
    void reject(const fs::path& path, ErrorCode code) {
        auto result = provider->acquire(path);
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == code);
    }
};

std::size_t fdCount() {
    return static_cast<std::size_t>(std::distance(fs::directory_iterator("/proc/self/fd"), fs::directory_iterator{}));
}

int waitChild(pid_t pid) {
    int status = 0;
    pid_t result;
    do {
        result = ::waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result != pid || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

int execProbe(const fs::path& root, bool success, const std::vector<int>& descriptors = {}) {
    std::vector<std::string> arguments{ INSTANCE_LOCK_PROBE, root.string(), success ? "1" : "0" };
    for (int fd : descriptors) arguments.push_back(std::to_string(fd));
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        ::alarm(5);
        ::execv(argv[0], argv.data());
        ::_exit(100);
    }
    // 子进程 alarm 对 exec 后同样生效；无无限等待的测试 helper。
    return waitChild(pid);
}
}  // namespace

BOOST_AUTO_TEST_CASE(lease_owns_lock_and_preserves_file_identity) {
    Fixture f;
    f.file(f.lock());
    struct stat before{}, after{};
    BOOST_REQUIRE(::stat(f.lock().c_str(), &before) == 0);
    auto first = f.provider->acquire(f.root / ".");
    BOOST_REQUIRE(first);
    BOOST_CHECK(first.value()->root() == f.root);
    f.reject(f.root, ErrorCode::Conflict);
    auto lease = std::move(first).value();
    f.provider.reset();
    BOOST_CHECK_EQUAL(execProbe(f.root, false), 0);
    lease.reset();
    BOOST_CHECK_EQUAL(execProbe(f.root, true), 0);
    BOOST_REQUIRE(::stat(f.lock().c_str(), &after) == 0);
    BOOST_CHECK_EQUAL(before.st_ino, after.st_ino);
    BOOST_CHECK_EQUAL(before.st_dev, after.st_dev);
    std::string content;
    std::ifstream(f.lock()) >> content;
    BOOST_CHECK_EQUAL(content, "preserve");
}

BOOST_AUTO_TEST_CASE(created_lock_is_private_and_reused) {
    Fixture f;
    auto first = f.provider->acquire(f.root);
    BOOST_REQUIRE(first);
    struct stat before{}, after{};
    BOOST_REQUIRE(::stat(f.lock().c_str(), &before) == 0);
    BOOST_CHECK_EQUAL(before.st_mode & 07777, 0600);
    BOOST_CHECK_EQUAL(before.st_uid, ::geteuid());
    first.value().reset();
    auto second = f.provider->acquire(f.root);
    BOOST_REQUIRE(second);
    BOOST_REQUIRE(::stat(f.lock().c_str(), &after) == 0);
    BOOST_CHECK_EQUAL(before.st_ino, after.st_ino);
}

BOOST_AUTO_TEST_CASE(fork_acquisition_is_independent_after_closing_inherited_lease) {
    Fixture f;
    auto first = f.provider->acquire(f.root);
    BOOST_REQUIRE(first);
    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        ::alarm(5);
        first.value().reset();
        auto result = f.provider->acquire(f.root);
        ::_exit(!result && result.error().code == ErrorCode::Conflict ? 0 : 1);
    }
    BOOST_CHECK_EQUAL(waitChild(pid), 0);
    first.value().reset();
    BOOST_CHECK_EQUAL(execProbe(f.root, true), 0);
}

BOOST_AUTO_TEST_CASE(all_lease_descriptors_close_on_exec) {
    Fixture f;
    auto first = f.provider->acquire(f.root);
    BOOST_REQUIRE(first);
    std::vector<int> descriptors;
    for (const auto& entry : fs::directory_iterator("/proc/self/fd")) {
        auto target = fs::read_symlink(entry.path());
        if (target == f.root || target == f.root / ".anof" || target == f.lock()) {
            int fd = std::stoi(entry.path().filename().string());
            BOOST_CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
            descriptors.push_back(fd);
        }
    }
    BOOST_REQUIRE_EQUAL(descriptors.size(), 3U);
    BOOST_CHECK_EQUAL(execProbe(f.root, false, descriptors), 0);
}

BOOST_AUTO_TEST_CASE(invalid_missing_and_symlink_paths) {
    Fixture f;
    f.reject("", ErrorCode::InvalidArgument);
    f.reject("relative", ErrorCode::InvalidArgument);
    f.reject(f.root / ".." / "instance", ErrorCode::InvalidArgument);
    f.reject(fs::path(f.root.string() + std::string(1, '\0') + "suffix"), ErrorCode::InvalidArgument);
    f.reject(f.base / "absent", ErrorCode::NotFound);
    fs::create_directory_symlink(f.root, f.base / "alias");
    f.reject(f.base / "alias", ErrorCode::IntegrityError);
    f.reject(f.base / "alias" / "child", ErrorCode::IntegrityError);
    fs::remove(f.root / ".anof");
    f.reject(f.root, ErrorCode::NotFound);
    fs::create_directory_symlink(f.base, f.root / ".anof");
    f.reject(f.root, ErrorCode::IntegrityError);
    BOOST_CHECK(!fs::exists(f.lock()));
}

BOOST_AUTO_TEST_CASE(directory_permissions_and_wrong_types) {
    Fixture f;
    for (auto path : { f.base, f.root, f.root / ".anof" }) {
        BOOST_REQUIRE(::chmod(path.c_str(), 0770) == 0);
        f.reject(f.root, ErrorCode::IntegrityError);
        BOOST_REQUIRE(::chmod(path.c_str(), 0700) == 0);
    }
    BOOST_REQUIRE(::chmod(f.root.c_str(), 01700) == 0);
    f.reject(f.root, ErrorCode::IntegrityError);
    BOOST_REQUIRE(::chmod(f.root.c_str(), 0700) == 0);
    fs::remove(f.root / ".anof");
    f.file(f.root / ".anof");
    f.reject(f.root, ErrorCode::IntegrityError);
}

BOOST_AUTO_TEST_CASE(unsafe_lock_files_never_open_or_truncate) {
    Fixture f;
    f.file(f.lock(), 0644);
    f.reject(f.root, ErrorCode::IntegrityError);
    BOOST_REQUIRE(::chmod(f.lock().c_str(), 0400) == 0);
    f.reject(f.root, ErrorCode::IntegrityError);
    BOOST_REQUIRE(::chmod(f.lock().c_str(), 04600) == 0);
    f.reject(f.root, ErrorCode::IntegrityError);
    fs::remove(f.lock());
    BOOST_REQUIRE(::mkfifo(f.lock().c_str(), 0600) == 0);
    f.reject(f.root, ErrorCode::IntegrityError);
    fs::remove(f.lock());
    fs::create_directory(f.lock());
    f.reject(f.root, ErrorCode::IntegrityError);
    fs::remove(f.lock());
    f.file(f.base / "target");
    fs::create_symlink(f.base / "target", f.lock());
    f.reject(f.root, ErrorCode::IntegrityError);
    fs::remove(f.lock());
    fs::create_hard_link(f.base / "target", f.lock());
    f.reject(f.root, ErrorCode::IntegrityError);
    BOOST_CHECK_EQUAL(fs::file_size(f.base / "target"), 8U);
}

BOOST_AUTO_TEST_CASE(failure_paths_do_not_leak_descriptors) {
    Fixture f;
    const auto before = fdCount();
    f.file(f.lock(), 0644);
    for (int i = 0; i < 32; ++i) f.reject(f.root, ErrorCode::IntegrityError);
    BOOST_CHECK_EQUAL(fdCount(), before);
    BOOST_REQUIRE(::chmod(f.lock().c_str(), 0600) == 0);
    auto held = f.provider->acquire(f.root);
    BOOST_REQUIRE(held);
    const auto locked = fdCount();
    for (int i = 0; i < 32; ++i) f.reject(f.root, ErrorCode::Conflict);
    BOOST_CHECK_EQUAL(fdCount(), locked);
    held.value().reset();
    BOOST_CHECK_EQUAL(fdCount(), before);
}

BOOST_AUTO_TEST_CASE(private_file_access_and_foreign_lease) {
    Fixture f;
    auto lease = f.provider->acquire(f.root);
    BOOST_REQUIRE(lease);
    auto directory = detail::instanceDirectory(*lease.value());
    BOOST_REQUIRE(directory);
    f.file(f.root / ".anof/config.toml", 0400);
    auto file = detail::openPrivateFile(directory.value(), "config.toml", false);
    BOOST_REQUIRE(file);
    BOOST_CHECK((::fcntl(file.value().get(), F_GETFD) & FD_CLOEXEC) != 0);
    BOOST_CHECK(!detail::openPrivateFile(directory.value(), "config.toml", true));
    BOOST_CHECK(!detail::openPrivateFile(directory.value(), "../config.toml", false));
    struct Fake final : InstanceLease {
        fs::path path{ "/" };
        const fs::path& root() const noexcept override { return path; }
    } fake;
    auto rejected = detail::instanceDirectory(fake);
    BOOST_REQUIRE(!rejected);
    BOOST_CHECK(rejected.error().code == ErrorCode::InvalidArgument);
}

BOOST_AUTO_TEST_CASE(filesystem_error_categories) {
    BOOST_CHECK(detail::filesystemError(ENOENT) == ErrorCode::NotFound);
    BOOST_CHECK(detail::filesystemError(ELOOP) == ErrorCode::IntegrityError);
    BOOST_CHECK(detail::filesystemError(ENOSPC) == ErrorCode::StorageFull);
    BOOST_CHECK(detail::filesystemError(EDQUOT) == ErrorCode::StorageFull);
    BOOST_CHECK(detail::filesystemError(EACCES) == ErrorCode::InternalError);
}

BOOST_AUTO_TEST_CASE(restrictive_umask_failure_preserves_file_and_releases_resources) {
    Fixture f;
    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        ::alarm(5);
        // 仅改变子进程 umask，不污染其他测试或生产线程。
        (void)::umask(0777);
        const auto before = fdCount();
        auto result = f.provider->acquire(f.root);
        ::_exit(!result && result.error().code == ErrorCode::IntegrityError && fdCount() == before ? 0 : 1);
    }
    BOOST_CHECK_EQUAL(waitChild(pid), 0);
    struct stat status{};
    BOOST_REQUIRE(::stat(f.lock().c_str(), &status) == 0);
    BOOST_CHECK_EQUAL(status.st_mode & 07777, 0);
    BOOST_REQUIRE(::chmod(f.lock().c_str(), 0600) == 0);
    BOOST_CHECK(f.provider->acquire(f.root));
}
