#define BOOST_TEST_MODULE TransferFiles
#include <anof/infrastructure/crypto/SodiumTransferIdGenerator.h>
#include <anof/infrastructure/storage/local/LocalTransferFileStore.h>
#include <anof/infrastructure/storage/local/internal/FileOperations.h>
#include <sys/stat.h>
#include <unistd.h>

#include <boost/test/unit_test.hpp>
#include <fstream>
namespace anof::infrastructure::storage {
namespace {
template <class T>
T id() { return T::parse("12345678-1234-4234-8234-123456789abc").value(); }
template <class T>
T take(FileResult<T> value) {
    BOOST_REQUIRE(std::holds_alternative<T>(value));
    return std::move(std::get<T>(value));
}
void ok(FileStatus value) { BOOST_REQUIRE(std::holds_alternative<std::monostate>(value)); }
struct Fixture {
    std::filesystem::path root;
    std::unique_ptr<TransferFileStore> store;
    Fixture() {
        char pattern[] = "/tmp/anof-transfer-files-XXXXXX";
        auto* p = mkdtemp(pattern);
        BOOST_REQUIRE(p);
        root = p;
        for (auto name : { ".anof", ".anof/uploads", ".anof/objects", ".anof/quarantine" }) BOOST_REQUIRE_EQUAL(mkdir((root / name).c_str(), 0700), 0);
        store = take(openLocalTransferFileStore(root));
    }
    ~Fixture() {
        store.reset();
        std::error_code e;
        std::filesystem::remove_all(root, e);
    }
};
}  // namespace
BOOST_AUTO_TEST_CASE(positioned_io_hash_publish_and_cleanup) {
    Fixture f;
    auto upload = id<UploadId>();
    auto blob = id<BlobId>();
    auto file = take(f.store->create(upload));
    BOOST_CHECK(std::get<TransferFileError>(f.store->create(upload)) == TransferFileError::AlreadyExists);
    TransferBlock block;
    block.bytes[0] = 'a';
    block.bytes[1] = 'b';
    block.bytes[2] = 'c';
    block.size = 3;
    ok(f.store->write(*file, {}, block));
    ok(f.store->syncData(*file));
    auto hash = take(f.store->beginHash());
    ok(f.store->updateHash(*hash, block));
    BOOST_CHECK_EQUAL(take(f.store->finishHash(std::move(hash))).toString(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    block.size = 0;
    ok(f.store->read(*file, {}, 3, block));
    BOOST_CHECK_EQUAL(block.bytes[2], 'c');
    BOOST_CHECK(std::get<TransferFileError>(f.store->read(*file, {}, 4, block)) == TransferFileError::Integrity);
    ok(f.store->publish(upload, blob));
    BOOST_CHECK(std::get<TransferFileError>(f.store->open(ObjectFile{ blob }, true)) == TransferFileError::Integrity);
    auto object = take(f.store->open(ObjectFile{ blob }, false));
    BOOST_CHECK_EQUAL(take(f.store->stat(*object)).size.value(), 3);
    auto inventory = take(f.store->inventory(InventoryArea::Objects, PageSize{}, {}));
    BOOST_REQUIRE_EQUAL(inventory.items.size(), 1);
    BOOST_CHECK(inventory.items[0].knownKey.has_value());
    ok(f.store->removeAndSync(ObjectFile{ blob }));
    ok(f.store->removeAndSync(ObjectFile{ blob }));
    BOOST_CHECK(std::get<TransferFileError>(f.store->stat(*object)) == TransferFileError::Integrity);
}
BOOST_AUTO_TEST_CASE(empty_multiblock_bounds_and_identity_validation) {
    Fixture f;
    auto file = take(f.store->create(id<UploadId>()));
    auto hash = take(f.store->beginHash());
    BOOST_CHECK_EQUAL(take(f.store->finishHash(std::move(hash))).toString(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    TransferBlock block;
    block.bytes.fill(42);
    block.size = block.bytes.size();
    hash = take(f.store->beginHash());
    for (int i = 0; i < 3; ++i) {
        ok(f.store->write(*file, UploadOffset::from(i * 65536).value(), block));
        ok(f.store->updateHash(*hash, block));
    }
    BOOST_CHECK_EQUAL(take(f.store->finishHash(std::move(hash))).toString(), "2a6ebc5b742c8b4c28a5d5f3d95da72fca0839959e8197ec7a8cf647743c255f");
    ok(f.store->syncData(*file));
    BOOST_CHECK_EQUAL(take(f.store->stat(*file)).size.value(), 3 * 65536);
    ok(f.store->read(*file, UploadOffset::from(65536).value(), 65536, block));
    BOOST_CHECK_EQUAL(block.bytes.back(), 42);
    ok(f.store->truncateAndSync(*file, UploadOffset::from(17).value()));
    BOOST_CHECK_EQUAL(take(f.store->stat(*file)).size.value(), 17);
    block.size = 65537;
    BOOST_CHECK(std::holds_alternative<TransferFileError>(f.store->write(*file, {}, block)));
    block.size = 1;
    BOOST_CHECK(std::holds_alternative<TransferFileError>(f.store->write(*file, UploadOffset::from(INT64_MAX).value(), block)));
    auto name = f.root / ".anof/uploads" / (id<UploadId>().toString() + ".part");
    BOOST_REQUIRE_EQUAL(link(name.c_str(), (f.root / "hardlink").c_str()), 0);
    BOOST_CHECK(std::holds_alternative<TransferFileError>(f.store->stat(*file)));
}
BOOST_AUTO_TEST_CASE(secure_ids_and_unknown_inventory) {
    auto generator = crypto::makeSodiumTransferIdGenerator();
    BOOST_REQUIRE(generator);
    auto a = generator.value()->uploadId();
    auto b = generator.value()->uploadId();
    auto c = generator.value()->blobId();
    BOOST_REQUIRE(a && b && c);
    BOOST_CHECK(a.value() != b.value());
    BOOST_CHECK_EQUAL(c.value().toString()[14], '4');
    Fixture f;
    for (auto name : { "a", "b", "c" }) {
        auto p = f.root / ".anof/uploads" / name;
        std::ofstream(p) << "x";
        BOOST_REQUIRE_EQUAL(chmod(p.c_str(), 0600), 0);
    }
    auto page = take(f.store->inventory(InventoryArea::Uploads, PageSize::from(2).value(), {}));
    BOOST_REQUIRE_EQUAL(page.items.size(), 2);
    BOOST_REQUIRE(page.next);
    BOOST_CHECK(!page.items[0].knownKey);
    auto tail = take(f.store->inventory(InventoryArea::Uploads, PageSize::from(2).value(), page.next));
    BOOST_REQUIRE_EQUAL(tail.items.size(), 1);
    BOOST_CHECK(!tail.next);
    BOOST_CHECK_EQUAL(tail.items[0].relativeName, "c");
    auto cap = take(f.store->capacity());
    BOOST_CHECK(cap.volume.value() >= cap.free.value());
    BOOST_REQUIRE_EQUAL(symlink("a", (f.root / ".anof/uploads/link").c_str()), 0);
    BOOST_CHECK(std::holds_alternative<TransferFileError>(f.store->inventory(InventoryArea::Uploads, PageSize{}, {})));
}
namespace {
class Faults final : public detail::FileOperations {
public:
    enum Mode { None,
                ShortIo,
                WriteFull,
                SyncFailure,
                TruncateFailure,
                PublishFailure,
                RemoveFailure,
                DirectorySyncFailure,
                AfterPublishSync } mode = None;
    bool interrupted = false;
    unsigned syncCalls = 0;
    unsigned failSyncAt = 0;
    std::vector<std::string> synced;
    ssize_t read(int fd, void* p, std::size_t n, off_t o) override {
        if (mode == ShortIo && !interrupted) {
            interrupted = true;
            errno = EINTR;
            return -1;
        }
        return FileOperations::read(fd, p, mode == ShortIo ? std::min(n, std::size_t{ 1 }) : n, o);
    }
    ssize_t write(int fd, const void* p, std::size_t n, off_t o) override {
        if (mode == WriteFull) {
            errno = ENOSPC;
            return -1;
        }
        if (mode == ShortIo && !interrupted) {
            interrupted = true;
            errno = EINTR;
            return -1;
        }
        return FileOperations::write(fd, p, mode == ShortIo ? std::min(n, std::size_t{ 1 }) : n, o);
    }
    int sync(int fd, bool data) override {
        ++syncCalls;
        synced.push_back(std::filesystem::read_symlink("/proc/self/fd/" + std::to_string(fd)).string());
        if (failSyncAt == syncCalls) { errno = EIO; return -1; }
        if (mode == SyncFailure || (mode == DirectorySyncFailure && !data)) {
            errno = EIO;
            return -1;
        }
        return FileOperations::sync(fd, data);
    }
    int truncate(int fd, off_t n) override {
        if (mode == TruncateFailure) {
            errno = EIO;
            return -1;
        }
        return FileOperations::truncate(fd, n);
    }
    int publish(int a, const char* b, int c, const char* d) override {
        if (mode == PublishFailure) {
            errno = EIO;
            return -1;
        }
        auto result = FileOperations::publish(a, b, c, d);
        if (mode == AfterPublishSync && result == 0) mode = DirectorySyncFailure;
        return result;
    }
    int remove(int fd, const char* n) override {
        if (mode == RemoveFailure) {
            errno = EIO;
            return -1;
        }
        return FileOperations::remove(fd, n);
    }
};
}  // namespace
BOOST_AUTO_TEST_CASE(short_io_and_failures_preserve_evidence_and_error_classes) {
    Fixture f;
    auto faults = std::make_shared<Faults>();
    f.store = take(detail::openLocalTransferFileStore(f.root, faults));
    auto file = take(f.store->create(id<UploadId>()));
    TransferBlock block;
    block.size = 3;
    block.bytes[0] = 1;
    block.bytes[1] = 2;
    block.bytes[2] = 3;
    faults->mode = Faults::ShortIo;
    ok(f.store->write(*file, {}, block));
    faults->interrupted = false;
    ok(f.store->read(*file, {}, 3, block));
    BOOST_CHECK_EQUAL(block.bytes[2], 3);
    faults->mode = Faults::WriteFull;
    BOOST_CHECK(std::get<TransferFileError>(f.store->write(*file, {}, block)) == TransferFileError::NoSpace);
    faults->mode = Faults::SyncFailure;
    BOOST_CHECK(std::get<TransferFileError>(f.store->syncData(*file)) == TransferFileError::UnknownEffect);
    faults->mode = Faults::TruncateFailure;
    BOOST_CHECK(std::get<TransferFileError>(f.store->truncateAndSync(*file, {})) == TransferFileError::UnknownEffect);
    faults->mode = Faults::PublishFailure;
    BOOST_CHECK(std::get<TransferFileError>(f.store->publish(id<UploadId>(), id<BlobId>())) == TransferFileError::UnknownEffect);
    faults->mode = Faults::None;
    BOOST_CHECK_EQUAL(take(f.store->stat(*file)).size.value(), 3);
    ok(f.store->publish(id<UploadId>(), id<BlobId>()));
    faults->mode = Faults::RemoveFailure;
    BOOST_CHECK(std::get<TransferFileError>(f.store->removeAndSync(ObjectFile{ id<BlobId>() })) == TransferFileError::UnknownEffect);
    faults->mode = Faults::DirectorySyncFailure;
    BOOST_CHECK(std::get<TransferFileError>(f.store->removeAndSync(ObjectFile{ id<BlobId>() })) == TransferFileError::UnknownEffect);
    // 删除已发生但目录同步失败，重试缺失文件仍必须同步父目录。
    auto calls = faults->syncCalls;
    faults->mode = Faults::None;
    ok(f.store->removeAndSync(ObjectFile{ id<BlobId>() }));
    BOOST_CHECK(faults->syncCalls > calls);
}
BOOST_AUTO_TEST_CASE(no_replace_and_directory_replacement_are_rejected) {
    Fixture f;
    auto file = take(f.store->create(id<UploadId>()));
    ok(f.store->publish(id<UploadId>(), id<BlobId>()));
    file = take(f.store->create(id<UploadId>()));
    BOOST_CHECK(std::get<TransferFileError>(f.store->publish(id<UploadId>(), id<BlobId>())) == TransferFileError::AlreadyExists);
    BOOST_REQUIRE(std::filesystem::exists(f.root / ".anof/uploads" / (id<UploadId>().toString() + ".part")));
    std::filesystem::rename(f.root / ".anof/uploads", f.root / ".anof/old");
    BOOST_REQUIRE_EQUAL(mkdir((f.root / ".anof/uploads").c_str(), 0700), 0);
    BOOST_CHECK(std::get<TransferFileError>(f.store->stat(*file)) == TransferFileError::Integrity);
}

BOOST_AUTO_TEST_CASE(post_publish_sync_failure_and_handle_cleanup_preserve_evidence) {
    Fixture f;
    auto faults = std::make_shared<Faults>();
    f.store = take(detail::openLocalTransferFileStore(f.root, faults));
    auto file = take(f.store->create(id<UploadId>()));
    // 先创建并同步分片，接缝只在 rename 成功后令目录同步失败。
    BOOST_REQUIRE_EQUAL(mkdir((f.root / ".anof/objects/12").c_str(), 0700), 0);
    faults->mode = Faults::AfterPublishSync;
    BOOST_CHECK(std::get<TransferFileError>(f.store->publish(id<UploadId>(), id<BlobId>())) == TransferFileError::UnknownEffect);
    faults->mode = Faults::None;
    BOOST_CHECK(!std::filesystem::exists(f.root / ".anof/uploads" / (id<UploadId>().toString() + ".part")));
    auto object = take(f.store->open(ObjectFile{ id<BlobId>() }, false));
    BOOST_CHECK_EQUAL(take(f.store->stat(*object)).size.value(), 0);
    auto count = [] { return std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator{}); };
    auto before = count();
    for (int i = 0; i < 16; ++i) {
        auto h = take(f.store->open(ObjectFile{ id<BlobId>() }, false));
        BOOST_REQUIRE(h);
    }
    BOOST_CHECK_EQUAL(count(), before);
}


BOOST_AUTO_TEST_CASE(publication_recovery_syncs_every_required_entry_and_retries) {
    Fixture f;
    auto faults = std::make_shared<Faults>();
    f.store = take(detail::openLocalTransferFileStore(f.root, faults));
    auto source = take(f.store->create(id<UploadId>()));
    TransferBlock block;
    block.size = 3;
    block.bytes[0] = 'a'; block.bytes[1] = 'b'; block.bytes[2] = 'c';
    ok(f.store->write(*source, {}, block));
    faults->mode = Faults::AfterPublishSync;
    BOOST_CHECK(std::get<TransferFileError>(f.store->publish(id<UploadId>(), id<BlobId>())) == TransferFileError::UnknownEffect);
    source.reset();
    f.store.reset();
    faults->mode = Faults::None;
    f.store = take(detail::openLocalTransferFileStore(f.root, faults));
    auto object = take(f.store->open(ObjectFile{ id<BlobId>() }, false));
    BOOST_CHECK_EQUAL(take(f.store->stat(*object)).size.value(), 3);
    ok(f.store->read(*object, {}, 3, block));
    auto hash = take(f.store->beginHash());
    ok(f.store->updateHash(*hash, block));
    BOOST_CHECK_EQUAL(take(f.store->finishHash(std::move(hash))).toString(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    for (unsigned point = 1; point <= 4; ++point) {
        faults->syncCalls = 0; faults->failSyncAt = point;
        BOOST_CHECK(std::get<TransferFileError>(f.store->syncPublication(id<UploadId>(), id<BlobId>())) == TransferFileError::UnknownEffect);
    }
    faults->failSyncAt = 0;
    const auto objectPath = f.root / ".anof/objects/12" / id<BlobId>().toString();
    struct stat before{}, after{};
    BOOST_REQUIRE_EQUAL(lstat(objectPath.c_str(), &before), 0);
    for (int i = 0; i < 2; ++i) {
        faults->synced.clear();
        ok(f.store->syncPublication(id<UploadId>(), id<BlobId>()));
        const std::vector<std::string> expected{objectPath.string(), (f.root/".anof/objects/12").string(),
            (f.root/".anof/uploads").string(), (f.root/".anof/objects").string()};
        BOOST_CHECK_EQUAL_COLLECTIONS(faults->synced.begin(), faults->synced.end(), expected.begin(), expected.end());
    }
    BOOST_REQUIRE_EQUAL(lstat(objectPath.c_str(), &after), 0);
    BOOST_CHECK_EQUAL(before.st_ino, after.st_ino);
    BOOST_CHECK_EQUAL(after.st_size, 3);
    auto otherSource = take(f.store->create(id<UploadId>()));
    BOOST_CHECK(std::get<TransferFileError>(f.store->syncPublication(id<UploadId>(), id<BlobId>())) == TransferFileError::Integrity);
    ok(f.store->removeAndSync(TemporaryFile{ id<UploadId>() }));
    BOOST_REQUIRE_EQUAL(chmod(objectPath.c_str(), 0644), 0);
    BOOST_CHECK(std::get<TransferFileError>(f.store->syncPublication(id<UploadId>(), id<BlobId>())) == TransferFileError::Integrity);
    BOOST_REQUIRE_EQUAL(chmod(objectPath.c_str(), 0600), 0);
    ok(f.store->removeAndSync(ObjectFile{ id<BlobId>() }));
    BOOST_CHECK(std::get<TransferFileError>(f.store->syncPublication(id<UploadId>(), id<BlobId>())) == TransferFileError::Integrity);
    otherSource = take(f.store->create(id<UploadId>()));
    BOOST_CHECK(std::get<TransferFileError>(f.store->syncPublication(id<UploadId>(), id<BlobId>())) == TransferFileError::Integrity);
}

}  // namespace anof::infrastructure::storage
