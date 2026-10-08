#define BOOST_TEST_MODULE InstanceConfiguration
#include <boost/test/unit_test.hpp>

#include <anof/infrastructure/config/TomlInstanceConfiguration.h>
#include <anof/infrastructure/system/LinuxInstanceLock.h>
#include <anof/infrastructure/system/internal/BoundedFileRead.h>
#include <anof/infrastructure/system/internal/InstanceFiles.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace anof;
using namespace anof::infrastructure;
namespace fs = std::filesystem;

namespace {
struct Fixture {
    fs::path root;
    std::unique_ptr<InstanceLockProvider> locks;
    std::unique_ptr<InstanceLease> lease;
    std::unique_ptr<InstanceConfigurationLoader> loader;
    Fixture() {
        BOOST_REQUIRE(::geteuid() != 0);
        char name[] = "/tmp/anof-config-XXXXXX";
        auto* directory = ::mkdtemp(name);
        BOOST_REQUIRE(directory);
        root = directory;
        BOOST_REQUIRE(::mkdir((root / ".anof").c_str(), 0700) == 0);
        auto provider = system::makeLinuxInstanceLockProvider();
        BOOST_REQUIRE(provider);
        locks = std::move(provider).value();
        auto acquired = locks->acquire(root);
        BOOST_REQUIRE(acquired);
        lease = std::move(acquired).value();
        auto factory = config::makeTomlInstanceConfigurationLoader();
        BOOST_REQUIRE(factory);
        loader = std::move(factory).value();
    }
    ~Fixture() {
        lease.reset();
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    fs::path path() const { return root / ".anof/config.toml"; }
    void write(const std::string& text) {
        std::ofstream output(path(), std::ios::binary | std::ios::trunc);
        output << text;
        output.close();
        BOOST_REQUIRE(output.good());
        BOOST_REQUIRE(::chmod(path().c_str(), 0600) == 0);
    }
    void invalid(const std::string& text) {
        write(text);
        auto result = loader->load(*lease);
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::InvalidArgument);
    }
    system::detail::OwnedFd open() {
        auto directory = system::detail::instanceDirectory(*lease);
        BOOST_REQUIRE(directory);
        auto file = system::detail::openPrivateFile(directory.value(), "config.toml", false);
        BOOST_REQUIRE(file);
        return std::move(file).value();
    }
};
const std::string enabled = "development_http = true\n";
std::size_t fdCount() {
    return static_cast<std::size_t>(std::distance(fs::directory_iterator("/proc/self/fd"), fs::directory_iterator{}));
}
}

BOOST_AUTO_TEST_CASE(defaults_explicit_opt_in_and_owned_values) {
    Fixture f;
    f.invalid("");
    f.invalid("# comment only\n");
    f.invalid("development_http = false\n");
    f.write(enabled);
    BOOST_REQUIRE(::chmod(f.path().c_str(), 0400) == 0);
    auto result = f.loader->load(*f.lease);
    BOOST_REQUIRE(result);
    const InstanceConfiguration defaults;
    BOOST_CHECK_EQUAL(result.value().listenAddress, defaults.listenAddress);
    BOOST_CHECK_EQUAL(result.value().port, defaults.port);
    BOOST_CHECK_EQUAL(result.value().metadataQueue, defaults.metadataQueue);
    BOOST_CHECK_EQUAL(result.value().httpInFlight, defaults.httpInFlight);
    BOOST_CHECK_EQUAL(result.value().shutdownGraceSeconds, defaults.shutdownGraceSeconds);
    BOOST_CHECK(result.value().developmentHttp);
    f.loader.reset();
    f.lease.reset();
    BOOST_CHECK_EQUAL(result.value().listenAddress, "127.0.0.1");
}

BOOST_AUTO_TEST_CASE(all_field_boundaries_and_toml_syntax) {
    Fixture f;
    for (const auto* text : {
        "port=0\nmetadata_queue=1\nhttp_in_flight=1\nshutdown_grace_seconds=0\n",
        "port=65535\nmetadata_queue=256\nhttp_in_flight=128\nshutdown_grace_seconds=30.0\n",
        "port=0x1f90\nmetadata_queue=2_56\nhttp_in_flight=128\nshutdown_grace_seconds=0.125\nlisten_address='127.0.0.1'\n" }) {
        f.write(enabled + text);
        auto result = f.loader->load(*f.lease);
        BOOST_REQUIRE(result);
    }
    f.write(enabled + "port=0\nmetadata_queue=1\nhttp_in_flight=1\nshutdown_grace_seconds=0.125\n");
    auto result = f.loader->load(*f.lease);
    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result.value().port, 0);
    BOOST_CHECK_EQUAL(result.value().metadataQueue, 1U);
    BOOST_CHECK_EQUAL(result.value().httpInFlight, 1U);
    BOOST_CHECK_EQUAL(result.value().shutdownGraceSeconds, 0.125);
}

BOOST_AUTO_TEST_CASE(negative_oversized_and_nonfinite_values) {
    Fixture f;
    for (const auto* text : {
        "port=-1", "port=65536", "port=9223372036854775807", "port=9223372036854775808",
        "metadata_queue=-1", "metadata_queue=0", "metadata_queue=257", "metadata_queue=9223372036854775807",
        "http_in_flight=-1", "http_in_flight=0", "http_in_flight=129", "http_in_flight=18446744073709551615",
        "shutdown_grace_seconds=-0.001", "shutdown_grace_seconds=30.001", "shutdown_grace_seconds=31",
        "shutdown_grace_seconds=nan", "shutdown_grace_seconds=+nan", "shutdown_grace_seconds=-nan",
        "shutdown_grace_seconds=inf", "shutdown_grace_seconds=+inf", "shutdown_grace_seconds=-inf",
        "shutdown_grace_seconds=1e999", "listen_address='0.0.0.0'", "listen_address='::1'",
        "listen_address='localhost'", "listen_address='127.0.0.1 '" }) f.invalid(enabled + text);
}

BOOST_AUTO_TEST_CASE(strict_types_unknown_and_duplicate_keys) {
    Fixture f;
    for (const auto* text : {
        "port=1.0", "port=true", "port='8080'", "metadata_queue=1.0", "http_in_flight=false",
        "listen_address=127", "shutdown_grace_seconds=true", "shutdown_grace_seconds='30'",
        "port=[]", "port={value=1}", "port=1979-05-27", "unknown=1", "[http]\nport=8080",
        "[port]\nvalue=8080", "port.value=8080", "[[port]]\nvalue=8080",
        "port=1\nport=2", "port=1\n\"port\"=2", "listen_address='127.0.0.1'\nlisten_address='127.0.0.1'" }) {
        f.invalid(enabled + text);
    }
    f.invalid("development_http='true'");
    f.invalid("development_http=1");
    f.invalid(enabled + enabled);
}

BOOST_AUTO_TEST_CASE(malformed_text_and_invalid_encoding) {
    Fixture f;
    f.invalid(enabled + "port = ");
    f.invalid(enabled + "listen_address = 'unterminated");
    f.invalid(enabled + std::string(1, '\0'));
    f.invalid(enabled + "# " + std::string(1, static_cast<char>(0xff)));
    f.invalid(enabled + "garbage trailing");
}

BOOST_AUTO_TEST_CASE(exact_file_limit_and_oversize) {
    Fixture f;
    std::string text = enabled + "#";
    text.resize(65536, 'x');
    f.write(text);
    BOOST_CHECK(f.loader->load(*f.lease));
    text += 'x';
    f.invalid(text);
}

BOOST_AUTO_TEST_CASE(read_growth_short_reads_and_interruptions) {
    Fixture f;
    f.write(enabled);
    auto file = f.open();
    bool grew = false;
    auto growth = system::detail::readBoundedFileWith(file.get(), 65536, ErrorCode::InvalidArgument,
        [&](int fd, void* output, std::size_t size) -> ssize_t {
            auto count = ::read(fd, output, size);
            if (!grew) {
                grew = true;
                std::ofstream(f.path(), std::ios::app) << std::string(65536, ' ');
            }
            return count;
        });
    BOOST_REQUIRE(!growth);
    BOOST_CHECK(growth.error().code == ErrorCode::InvalidArgument);
    file = system::detail::OwnedFd{};
    f.write(enabled);
    file = f.open();
    bool interrupted = false;
    auto shortRead = system::detail::readBoundedFileWith(file.get(), 65536, ErrorCode::InvalidArgument,
        [&](int fd, void* output, std::size_t size) -> ssize_t {
            if (!interrupted) { interrupted = true; errno = EINTR; return -1; }
            return ::read(fd, output, std::min(size, std::size_t{ 3 }));
        });
    BOOST_REQUIRE(shortRead);
    BOOST_CHECK_EQUAL(shortRead.value(), enabled);
}

BOOST_AUTO_TEST_CASE(read_error_classification_and_invalid_fd) {
    Fixture f;
    f.write(enabled);
    auto file = f.open();
    auto result = system::detail::readBoundedFileWith(file.get(), 65536, ErrorCode::InvalidArgument,
        [](int, void*, std::size_t) -> ssize_t { errno = EIO; return -1; });
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == ErrorCode::InternalError);
    auto invalid = system::detail::readBoundedFile(-1, 65536, ErrorCode::InvalidArgument);
    BOOST_REQUIRE(!invalid);
    BOOST_CHECK(invalid.error().code == ErrorCode::InternalError);
}

BOOST_AUTO_TEST_CASE(unsafe_files_missing_and_foreign_lease) {
    Fixture f;
    auto missing = f.loader->load(*f.lease);
    BOOST_REQUIRE(!missing);
    BOOST_CHECK(missing.error().code == ErrorCode::NotFound);
    f.write(enabled);
    BOOST_REQUIRE(::chmod(f.path().c_str(), 0644) == 0);
    auto unsafe = f.loader->load(*f.lease);
    BOOST_REQUIRE(!unsafe);
    BOOST_CHECK(unsafe.error().code == ErrorCode::IntegrityError);
    fs::remove(f.path());
    BOOST_REQUIRE(::mkfifo(f.path().c_str(), 0600) == 0);
    BOOST_CHECK(!f.loader->load(*f.lease));
    fs::remove(f.path());
    fs::create_directory(f.path());
    BOOST_CHECK(!f.loader->load(*f.lease));
    fs::remove(f.path());
    fs::create_symlink("instance.lock", f.path());
    BOOST_CHECK(!f.loader->load(*f.lease));
    fs::remove(f.path());
    fs::create_hard_link(f.root / ".anof/instance.lock", f.path());
    BOOST_CHECK(!f.loader->load(*f.lease));
    struct Fake final : InstanceLease {
        fs::path path{ "/" };
        const fs::path& root() const noexcept override { return path; }
    } fake;
    auto foreign = f.loader->load(fake);
    BOOST_REQUIRE(!foreign);
    BOOST_CHECK(foreign.error().code == ErrorCode::InvalidArgument);
}

BOOST_AUTO_TEST_CASE(failed_load_preserves_lease_and_closes_resources) {
    Fixture f;
    const auto before = fdCount();
    for (int i = 0; i < 16; ++i) f.invalid(enabled + "port=-1");
    BOOST_CHECK_EQUAL(fdCount(), before);
    auto conflict = f.locks->acquire(f.root);
    BOOST_REQUIRE(!conflict);
    BOOST_CHECK(conflict.error().code == ErrorCode::Conflict);
    BOOST_CHECK(!fs::exists(f.root / ".anof/metadata.sqlite3"));
    f.write(enabled);
    BOOST_CHECK(f.loader->load(*f.lease));
    BOOST_CHECK_EQUAL(fdCount(), before);
}
