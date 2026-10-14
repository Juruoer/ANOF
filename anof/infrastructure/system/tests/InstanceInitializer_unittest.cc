#define BOOST_TEST_MODULE AnofInstanceInitializer
#include <boost/test/unit_test.hpp>
#include <anof/infrastructure/system/LinuxInstanceInitializer.h>
#include <anof/infrastructure/system/LinuxInstanceLock.h>
#include <anof/infrastructure/system/internal/InitializationFiles.h>
#include <anof/infrastructure/system/internal/InstanceFiles.h>
#include <anof/infrastructure/config/TomlInstanceConfiguration.h>
#include <anof/infrastructure/config/internal/ConfigurationText.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace anof::infrastructure::system {
namespace {
const InitializationIdentity identity{
    InstanceId::parse("11111111-1111-4111-8111-111111111111").value(),
    OperationId::parse("22222222-2222-4222-8222-222222222222").value() };
InstanceConfiguration config() { InstanceConfiguration c; c.developmentHttp = true; c.shutdownGraceSeconds = 1.125; return c; }
template <typename T>
void errorIs(const InitResult<T>& result, InitError code, InitStage stage) {
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == code);
    BOOST_CHECK(result.error().stage == stage);
}
struct Tree {
    std::filesystem::path root;
    Tree() {
        char pattern[] = "/tmp/anof-init-files-XXXXXX";
        const auto* path = ::mkdtemp(pattern);
        if (!path) throw std::runtime_error("temporary directory failed");
        root = path;
    }
    ~Tree() { std::error_code error; std::filesystem::remove_all(root, error); }
    std::filesystem::path file(std::string_view name) const { return root / ".anof" / name; }
    std::string read(std::string_view name) const {
        std::ifstream input(file(name), std::ios::binary);
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }
    void put(std::string_view name, const std::string& text) {
        detail::OwnedFd fd(::open(file(name).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600));
        BOOST_REQUIRE(fd.get() >= 0);
        BOOST_REQUIRE_EQUAL(::write(fd.get(), text.data(), text.size()), static_cast<ssize_t>(text.size()));
    }
};
std::unique_ptr<InstanceInitializer> initializer() {
    auto result = makeLinuxInstanceInitializer();
    BOOST_REQUIRE(result && result.value());
    return std::move(result).value();
}
std::unique_ptr<InitializationLease> create(InstanceInitializer& init, const Tree& tree, bool withConfig = true) {
    auto result = init.create(tree.root, identity);
    BOOST_REQUIRE(result && result.value());
    auto lease = std::move(result).value();
    if (withConfig) BOOST_REQUIRE(lease->writeConfiguration(config()));
    return lease;
}
struct stat fileStatus(const std::filesystem::path& path) {
    struct stat result{};
    BOOST_REQUIRE_EQUAL(::lstat(path.c_str(), &result), 0);
    return result;
}
void probe(const Tree& tree, bool success) {
    const pid_t child = ::fork();
    BOOST_REQUIRE(child >= 0);
    if (child == 0) {
        ::execl(INSTANCE_LOCK_PROBE, INSTANCE_LOCK_PROBE, tree.root.c_str(), success ? "1" : "0", nullptr);
        ::_exit(99);
    }
    int result = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &result, 0); } while (waited < 0 && errno == EINTR);
    BOOST_REQUIRE_EQUAL(waited, child);
    BOOST_REQUIRE(WIFEXITED(result));
    BOOST_CHECK_EQUAL(WEXITSTATUS(result), 0);
}
int syncCalls = 0, failSync = 0, writeCalls = 0, writeMode = 0, renameMode = 0, unlinkMode = 0;
ssize_t writeHook(int fd, const void* data, std::size_t size) {
    ++writeCalls;
    if (writeMode == 1 && writeCalls == 1) { errno = EINTR; return -1; }
    if (writeMode == 2) { errno = ENOSPC; return -1; }
    if (writeMode == 3) return 0;
    return ::write(fd, data, writeMode == 1 ? std::min<std::size_t>(3, size) : size);
}
int syncHook(int fd) {
    if (++syncCalls == failSync) { errno = EIO; return -1; }
    return ::fsync(fd);
}
int renameHook(int a, const char* x, int b, const char* y) {
    if (renameMode == 1) { errno = EACCES; return -1; }
    const int result = detail::initializationFileOps().renameNoReplace(a, x, b, y);
    if (renameMode == 2 && result == 0) { errno = EIO; return -1; }
    return result;
}
int unlinkHook(int a, const char* name, int flags) {
    if (unlinkMode) { errno = EACCES; return -1; }
    return ::unlinkat(a, name, flags);
}
std::unique_ptr<InstanceInitializer> faultInitializer() {
    syncCalls = failSync = writeCalls = writeMode = renameMode = unlinkMode = 0;
    auto result = detail::makeLinuxInstanceInitializerWith({ writeHook, syncHook, renameHook, unlinkHook });
    BOOST_REQUIRE(result);
    return std::move(result).value();
}
std::string temporary(std::string_view name) { return "." + std::string(name) + "." + identity.operationId.toString() + ".tmp"; }
}  // namespace

BOOST_AUTO_TEST_CASE(create_holds_same_stable_lease_and_writes_only_current_files) {
    Tree tree;
    auto init = initializer();
    auto lease = create(*init, tree, false);
    BOOST_CHECK(lease->identity().instanceId == identity.instanceId);
    BOOST_CHECK(lease->identity().operationId == identity.operationId);
    BOOST_CHECK(lease->lease().root() == tree.root);
    BOOST_CHECK_EQUAL(fileStatus(tree.root / ".anof").st_mode & 07777, 0700);
    for (const auto* name : { "instance.lock", "initialization.pending" }) {
        const auto info = fileStatus(tree.file(name));
        BOOST_CHECK(S_ISREG(info.st_mode));
        BOOST_CHECK_EQUAL(info.st_mode & 07777, 0600);
        BOOST_CHECK_EQUAL(info.st_nlink, 1);
    }
    for (auto fd : { detail::instanceDirectory(lease->lease()), detail::instanceRootDirectory(lease->lease()), detail::instanceLockFile(lease->lease()) }) {
        BOOST_REQUIRE(fd);
        BOOST_CHECK(::fcntl(fd.value(), F_GETFD) & FD_CLOEXEC);
    }
    auto parsed = detail::parsePending(tree.read("initialization.pending"), InitStage::Verify);
    BOOST_REQUIRE(parsed);
    const auto inode = fileStatus(tree.file("instance.lock")).st_ino;
    errorIs(init->create(tree.root, identity), InitError::AlreadyExists, InitStage::CreateDirectory);
    errorIs(init->openPending(tree.root), InitError::Busy, InitStage::AcquireLock);
    auto loader = config::makeTomlInstanceConfigurationLoader();
    BOOST_REQUIRE(loader);
    errorIs(lease->publishFormat(), InitError::IncompleteInitialization, InitStage::PublishFormat);
    BOOST_REQUIRE(lease->writeConfiguration(config()));
    auto loaded = loader.value()->load(lease->lease());
    BOOST_REQUIRE(loaded);
    BOOST_CHECK(config::detail::sameConfiguration(loaded.value(), config()));
    BOOST_CHECK_EQUAL(fileStatus(tree.file("config.toml")).st_mode & 07777, 0600);
    errorIs(lease->writeConfiguration(config()), InitError::AlreadyExists, InitStage::WriteConfiguration);
    errorIs(lease->clearPending(), InitError::IncompleteInitialization, InitStage::ClearPending);
    init.reset();
    probe(tree, false);
    lease.reset();
    probe(tree, true);
    BOOST_CHECK_EQUAL(fileStatus(tree.file("instance.lock")).st_ino, inode);
    BOOST_CHECK(!std::filesystem::exists(tree.file("metadata.sqlite3")));
    BOOST_CHECK(!std::filesystem::exists(tree.file("objects")));
}

BOOST_AUTO_TEST_CASE(file_side_finish_preserves_configuration_and_published_marker) {
    Tree tree;
    auto init = initializer();
    auto lease = create(*init, tree);
    const auto configBytes = tree.read("config.toml");
    lease.reset();
    auto resumed = init->openPending(tree.root);
    BOOST_REQUIRE(resumed);
    errorIs(resumed.value()->writeConfiguration(config()), InitError::AlreadyExists, InitStage::WriteConfiguration);
    BOOST_REQUIRE(resumed.value()->publishFormat());
    const auto first = fileStatus(tree.file("instance.json"));
    const auto marker = tree.read("instance.json");
    BOOST_REQUIRE(resumed.value()->publishFormat());
    BOOST_CHECK_EQUAL(fileStatus(tree.file("instance.json")).st_ino, first.st_ino);
    BOOST_CHECK_EQUAL(fileStatus(tree.file("instance.json")).st_mtim.tv_nsec, first.st_mtim.tv_nsec);
    BOOST_CHECK(tree.read("instance.json") == marker);
    resumed.value().reset();
    resumed = init->openPending(tree.root);
    BOOST_REQUIRE(resumed);
    BOOST_REQUIRE(resumed.value()->publishFormat());
    BOOST_REQUIRE(resumed.value()->clearPending());
    BOOST_CHECK(!std::filesystem::exists(tree.file("initialization.pending")));
    BOOST_CHECK(tree.read("config.toml") == configBytes);
    resumed.value().reset();
    errorIs(init->openPending(tree.root), InitError::IncompleteInitialization, InitStage::Verify);
    errorIs(init->create(tree.root, identity), InitError::AlreadyExists, InitStage::CreateDirectory);
}

BOOST_AUTO_TEST_CASE(root_must_be_existing_empty_owned_private_and_not_a_symlink) {
    Tree tree;
    auto init = initializer();
    errorIs(init->create("relative", identity), InitError::InvalidInput, InitStage::Validate);
    errorIs(init->create(tree.root / "missing", identity), InitError::NotFound, InitStage::CreateDirectory);
    BOOST_REQUIRE_EQUAL(::chmod(tree.root.c_str(), 0755), 0);
    errorIs(init->create(tree.root, identity), InitError::IntegrityError, InitStage::CreateDirectory);
    BOOST_REQUIRE_EQUAL(::chmod(tree.root.c_str(), 0700), 0);
    std::filesystem::create_symlink(tree.root, tree.root / "link");
    errorIs(init->create(tree.root / "link", identity), InitError::IntegrityError, InitStage::CreateDirectory);
    errorIs(init->create(tree.root, identity), InitError::InvalidInput, InitStage::CreateDirectory);
    BOOST_CHECK(!std::filesystem::exists(tree.root / ".anof"));
    std::filesystem::create_symlink("missing", tree.root / ".anof");
    errorIs(init->create(tree.root, identity), InitError::AlreadyExists, InitStage::CreateDirectory);
}

BOOST_AUTO_TEST_CASE(umask_is_not_repaired_and_incomplete_directory_is_retained) {
    Tree tree;
    auto init = initializer();
    const mode_t saved = ::umask(0777);
    auto result = init->create(tree.root, identity);
    ::umask(saved);
    errorIs(result, InitError::IntegrityError, InitStage::CreateDirectory);
    BOOST_CHECK_EQUAL(fileStatus(tree.root / ".anof").st_mode & 07777, 0);
    // 仅恢复测试自己创建目录的可清理性；生产实现没有 chmod 修复。
    BOOST_REQUIRE_EQUAL(::chmod((tree.root / ".anof").c_str(), 0700), 0);
    errorIs(init->create(tree.root, identity), InitError::AlreadyExists, InitStage::CreateDirectory);
}

BOOST_AUTO_TEST_CASE(pending_json_is_bounded_exact_canonical_and_versioned) {
    const auto valid = detail::pendingText(identity);
    for (const auto& text : { std::string{}, valid + "{}", "\xef\xbb\xbf" + valid, std::string(4097, ' '),
                             std::string("[]"), std::string("{\"version\":1,\"version\":1}"),
                             std::string("{\"version\":1.0}"), std::string("{\"instance_id\":[]}"),
                             std::string("{\"path\":\"../other\"}") }) {
        errorIs(detail::parsePending(text, InitStage::Verify), InitError::IntegrityError, InitStage::Verify);
    }
    for (const auto* key : { "version", "target_schema", "target_format" }) {
        auto text = valid;
        const auto offset = text.find(std::string("\"") + key + "\":") + std::strlen(key) + 3;
        text[offset] = '9';
        errorIs(detail::parsePending(text, InitStage::Verify), InitError::UnsupportedFormat, InitStage::Verify);
    }
    auto duplicate = valid;
    duplicate.insert(1, "\"instance_id\":\"11111111-1111-4111-8111-111111111111\",");
    errorIs(detail::parsePending(duplicate, InitStage::Verify), InitError::IntegrityError, InitStage::Verify);
    auto unknown = valid;
    unknown.insert(1, "\"unknown\":0,");
    errorIs(detail::parsePending(unknown, InitStage::Verify), InitError::IntegrityError, InitStage::Verify);
    auto badId = valid;
    badId.replace(badId.find(identity.instanceId.toString()), 36, "AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA");
    errorIs(detail::parsePending(badId, InitStage::Verify), InitError::IntegrityError, InitStage::Verify);
    BOOST_REQUIRE(detail::parsePending(valid + std::string(4096 - valid.size(), ' '), InitStage::Verify));
}

BOOST_AUTO_TEST_CASE(open_pending_requires_configuration_and_never_recreates_lock) {
    Tree tree;
    auto init = initializer();
    auto lease = create(*init, tree, false);
    lease.reset();
    errorIs(init->openPending(tree.root), InitError::IncompleteInitialization, InitStage::Verify);
    tree.put("config.toml", "development_http=true\n");
    errorIs(init->openPending(tree.root), InitError::InvalidInput, InitStage::Verify);
    tree.put("config.toml", config::detail::serializeConfiguration(config()).value());
    BOOST_REQUIRE(init->openPending(tree.root));
    BOOST_REQUIRE_EQUAL(::unlink(tree.file("instance.lock").c_str()), 0);
    errorIs(init->openPending(tree.root), InitError::IncompleteInitialization, InitStage::AcquireLock);
    BOOST_CHECK(!std::filesystem::exists(tree.file("instance.lock")));
}

BOOST_AUTO_TEST_CASE(pending_and_configuration_path_types_and_bounds_fail_closed) {
    for (int kind = 0; kind < 5; ++kind) {
        Tree tree;
        auto init = initializer();
        auto lease = create(*init, tree);
        lease.reset();
        const auto pending = tree.read("initialization.pending");
        BOOST_REQUIRE_EQUAL(::unlink(tree.file("initialization.pending").c_str()), 0);
        if (kind == 0) std::filesystem::create_symlink("config.toml", tree.file("initialization.pending"));
        if (kind == 1) BOOST_REQUIRE_EQUAL(::mkfifo(tree.file("initialization.pending").c_str(), 0600), 0);
        if (kind == 2) { tree.put("initialization.pending", pending); BOOST_REQUIRE_EQUAL(::link(tree.file("initialization.pending").c_str(), tree.file("alias").c_str()), 0); }
        if (kind == 3) tree.put("initialization.pending", std::string(4097, 'x'));
        if (kind == 4) { tree.put("initialization.pending", pending); BOOST_REQUIRE_EQUAL(::chmod(tree.file("initialization.pending").c_str(), 0644), 0); }
        errorIs(init->openPending(tree.root), InitError::IntegrityError, InitStage::Verify);
        BOOST_CHECK(std::filesystem::exists(tree.file("instance.lock")));
    }
    Tree tree;
    auto init = initializer();
    auto lease = create(*init, tree);
    lease.reset();
    tree.put("config.toml", std::string(65537, 'x'));
    errorIs(init->openPending(tree.root), InitError::IntegrityError, InitStage::Verify);
}

BOOST_AUTO_TEST_CASE(configuration_validation_and_no_overwrite_precede_publication) {
    Tree tree;
    auto init = initializer();
    auto lease = create(*init, tree, false);
    errorIs(lease->writeConfiguration({}), InitError::InvalidInput, InitStage::WriteConfiguration);
    BOOST_CHECK(!std::filesystem::exists(tree.file("config.toml")));
    tree.put("config.toml", "preserve this file");
    errorIs(lease->writeConfiguration(config()), InitError::AlreadyExists, InitStage::WriteConfiguration);
    BOOST_CHECK(tree.read("config.toml") == "preserve this file");
}

BOOST_AUTO_TEST_CASE(format_mismatch_unknown_fields_and_version_never_overwrite) {
    for (const auto& marker : {
        std::string("{\"instance_id\":\"33333333-3333-4333-8333-333333333333\",\"format_version\":2}"),
        std::string("{\"instance_id\":\"11111111-1111-4111-8111-111111111111\",\"format_version\":2,\"extra\":0}"),
        std::string("{\"instance_id\":\"11111111-1111-4111-8111-111111111111\",\"format_version\":1}") }) {
        Tree tree;
        auto init = initializer();
        auto lease = create(*init, tree);
        tree.put("instance.json", marker);
        const auto result = lease->publishFormat();
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().stage == InitStage::PublishFormat);
        BOOST_CHECK(tree.read("instance.json") == marker);
        BOOST_CHECK(std::filesystem::exists(tree.file("initialization.pending")));
        lease.reset();
        BOOST_CHECK(!init->openPending(tree.root));
    }
}

BOOST_AUTO_TEST_CASE(changed_pending_configuration_and_lock_are_not_trusted) {
    for (int change = 0; change < 3; ++change) {
        Tree tree;
        auto init = initializer();
        auto lease = create(*init, tree);
        BOOST_REQUIRE(lease->publishFormat());
        if (change == 0) {
            tree.put("replacement", detail::pendingText(identity));
            BOOST_REQUIRE_EQUAL(::rename(tree.file("replacement").c_str(), tree.file("initialization.pending").c_str()), 0);
        } else if (change == 1) {
            auto changed = config(); changed.port = 9999;
            tree.put("config.toml", config::detail::serializeConfiguration(changed).value());
        } else {
            BOOST_REQUIRE_EQUAL(::unlink(tree.file("instance.lock").c_str()), 0);
            tree.put("instance.lock", "");
        }
        errorIs(lease->clearPending(), InitError::IntegrityError, InitStage::ClearPending);
        BOOST_CHECK(std::filesystem::exists(tree.file("initialization.pending")));
    }
}

BOOST_AUTO_TEST_CASE(short_writes_and_eintr_complete_without_unbounded_data) {
    Tree tree;
    auto init = faultInitializer();
    writeMode = 1;
    auto lease = create(*init, tree);
    BOOST_REQUIRE(lease->publishFormat());
    BOOST_REQUIRE(lease->clearPending());
    BOOST_CHECK(writeCalls > 3);
}

BOOST_AUTO_TEST_CASE(write_failures_preserve_pending_and_partial_files) {
    for (int mode : { 2, 3 }) {
        Tree tree;
        auto init = faultInitializer();
        auto lease = create(*init, tree, false);
        writeMode = mode;
        errorIs(lease->writeConfiguration(config()), mode == 2 ? InitError::StorageFull : InitError::IoError, InitStage::WriteConfiguration);
        BOOST_CHECK(std::filesystem::exists(tree.file("initialization.pending")));
        BOOST_CHECK(std::filesystem::exists(tree.file(temporary("config.toml"))));
        BOOST_CHECK(!std::filesystem::exists(tree.file("config.toml")));
        lease.reset();
        errorIs(init->openPending(tree.root), InitError::IncompleteInitialization, InitStage::Verify);
    }
}

BOOST_AUTO_TEST_CASE(pending_sync_failure_classifies_before_and_after_publication) {
    for (int point = 1; point <= 4; ++point) {
        Tree tree;
        auto init = faultInitializer();
        failSync = point;
        errorIs(init->create(tree.root, identity), point <= 2 ? InitError::IoError : InitError::PublicationOutcomeUnknown, InitStage::WritePending);
        BOOST_CHECK(std::filesystem::exists(tree.root / ".anof"));
        BOOST_CHECK(std::filesystem::exists(tree.file("initialization.pending")) == (point >= 3));
        errorIs(init->create(tree.root, identity), InitError::AlreadyExists, InitStage::CreateDirectory);
    }
}

BOOST_AUTO_TEST_CASE(format_sync_failures_are_explicitly_finishable_without_overwrite) {
    for (int point = 1; point <= 3; ++point) {
        Tree tree;
        auto init = faultInitializer();
        auto lease = create(*init, tree);
        syncCalls = 0; failSync = point;
        errorIs(lease->publishFormat(), point == 1 ? InitError::IoError : InitError::PublicationOutcomeUnknown, InitStage::PublishFormat);
        BOOST_CHECK(std::filesystem::exists(tree.file("instance.json")) == (point >= 2));
        errorIs(lease->clearPending(), InitError::IncompleteInitialization, InitStage::ClearPending);
        lease.reset();
        auto normal = initializer();
        auto resumed = normal->openPending(tree.root);
        BOOST_REQUIRE(resumed);
        BOOST_REQUIRE(resumed.value()->publishFormat());
        BOOST_REQUIRE(resumed.value()->clearPending());
    }
}

BOOST_AUTO_TEST_CASE(rename_failure_and_unknown_outcome_preserve_recoverable_evidence) {
    for (int mode : { 1, 2 }) {
        Tree tree;
        auto init = faultInitializer();
        auto lease = create(*init, tree);
        renameMode = mode;
        errorIs(lease->publishFormat(), mode == 1 ? InitError::IoError : InitError::PublicationOutcomeUnknown, InitStage::PublishFormat);
        BOOST_CHECK(std::filesystem::exists(tree.file("initialization.pending")));
        lease.reset();
        auto normal = initializer();
        auto resumed = normal->openPending(tree.root);
        BOOST_REQUIRE(resumed);
        BOOST_REQUIRE(resumed.value()->publishFormat());
        BOOST_REQUIRE(resumed.value()->clearPending());
    }
}

BOOST_AUTO_TEST_CASE(pending_removal_failure_and_sync_unknown_are_distinct) {
    Tree tree;
    auto init = faultInitializer();
    auto lease = create(*init, tree);
    BOOST_REQUIRE(lease->publishFormat());
    unlinkMode = 1;
    errorIs(lease->clearPending(), InitError::IoError, InitStage::ClearPending);
    BOOST_CHECK(std::filesystem::exists(tree.file("initialization.pending")));
    unlinkMode = 0; syncCalls = 0; failSync = 1;
    errorIs(lease->clearPending(), InitError::PublicationOutcomeUnknown, InitStage::ClearPending);
    BOOST_CHECK(!std::filesystem::exists(tree.file("initialization.pending")));
    BOOST_CHECK(std::filesystem::exists(tree.file("instance.json")));
}

BOOST_AUTO_TEST_CASE(invalid_format_temporary_is_preserved) {
    Tree tree;
    auto init = initializer();
    auto lease = create(*init, tree);
    const auto name = temporary("instance.json");
    tree.put(name, "unfinished");
    const auto inode = fileStatus(tree.file(name)).st_ino;
    errorIs(lease->publishFormat(), InitError::IntegrityError, InitStage::PublishFormat);
    BOOST_CHECK_EQUAL(tree.read(name), "unfinished");
    BOOST_CHECK_EQUAL(fileStatus(tree.file(name)).st_ino, inode);
    BOOST_CHECK(!std::filesystem::exists(tree.file("instance.json")));
    BOOST_CHECK(std::filesystem::exists(tree.file("initialization.pending")));
}

BOOST_AUTO_TEST_CASE(configuration_sync_failures_preserve_published_or_partial_state) {
    for (int point = 1; point <= 3; ++point) {
        Tree tree;
        auto init = faultInitializer();
        auto lease = create(*init, tree, false);
        syncCalls = 0; failSync = point;
        errorIs(lease->writeConfiguration(config()), point == 1 ? InitError::IoError : InitError::PublicationOutcomeUnknown, InitStage::WriteConfiguration);
        BOOST_CHECK(std::filesystem::exists(tree.file("config.toml")) == (point >= 2));
        lease.reset();
        auto normal = initializer();
        auto resumed = normal->openPending(tree.root);
        if (point == 1) {
            errorIs(resumed, InitError::IncompleteInitialization, InitStage::Verify);
            BOOST_CHECK(std::filesystem::exists(tree.file(temporary("config.toml"))));
        } else {
            BOOST_REQUIRE(resumed);
            BOOST_REQUIRE(resumed.value()->publishFormat());
            BOOST_REQUIRE(resumed.value()->clearPending());
        }
    }
}

BOOST_AUTO_TEST_CASE(terminated_creator_leaves_pending_and_releases_stable_lock) {
    for (bool published : { false, true }) {
        Tree tree;
        const pid_t child = ::fork();
        BOOST_REQUIRE(child >= 0);
        if (child == 0) {
            ::alarm(5);
            auto init = makeLinuxInstanceInitializer();
            if (!init) ::_exit(10);
            auto lease = init.value()->create(tree.root, identity);
            if (!lease || !lease.value()->writeConfiguration(config())) ::_exit(11);
            if (published && !lease.value()->publishFormat()) ::_exit(12);
            // 绕过所有析构，覆盖格式发布前后进程终止；不代表硬件断电。
            ::_exit(0);
        }
        int result = 0;
        BOOST_REQUIRE_EQUAL(::waitpid(child, &result, 0), child);
        BOOST_REQUIRE(WIFEXITED(result) && WEXITSTATUS(result) == 0);
        auto init = initializer();
        auto resumed = init->openPending(tree.root);
        BOOST_REQUIRE(resumed);
        BOOST_REQUIRE(resumed.value()->publishFormat());
        BOOST_REQUIRE(resumed.value()->clearPending());
    }
}
}  // namespace anof::infrastructure::system
