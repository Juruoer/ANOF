#include <anof/infrastructure/persistence/sqlite/tests/SqliteFaultVfs.h>

#include <cstring>
#include <new>
#include <stdexcept>

namespace anof::infrastructure::sqlite::testing {

struct SqliteFaultVfs::File {
    sqlite3_file base{};
    sqlite3_file* real = nullptr;
    SqliteFaultVfs* owner = nullptr;
    bool wal = false;
    static File& from(sqlite3_file* file) { return *reinterpret_cast<File*>(file); }
    static sqlite3_file* unwrap(sqlite3_file* file) { return from(file).real; }
    static const sqlite3_io_methods methods;
};

const sqlite3_io_methods SqliteFaultVfs::File::methods{
    2,
    [](sqlite3_file* file) {
        auto& f = from(file);
        const int result = f.real->pMethods->xClose(f.real);
        sqlite3_free(f.real);
        f.real = nullptr;
        f.base.pMethods = nullptr;
        return result;
    },
    [](sqlite3_file* file, void* buffer, int amount, sqlite3_int64 offset) {
        auto* real = unwrap(file); return real->pMethods->xRead(real, buffer, amount, offset);
    },
    [](sqlite3_file* file, const void* buffer, int amount, sqlite3_int64 offset) {
        auto& f = from(file);
        if (f.wal && f.owner->fail(Failure::WalWrite)) return SQLITE_IOERR_WRITE;
        return f.real->pMethods->xWrite(f.real, buffer, amount, offset);
    },
    [](sqlite3_file* file, sqlite3_int64 size) {
        auto* real = unwrap(file); return real->pMethods->xTruncate(real, size);
    },
    [](sqlite3_file* file, int flags) {
        auto& f = from(file);
        if (f.wal && f.owner->fail(Failure::WalSync)) return SQLITE_IOERR_FSYNC;
        return f.real->pMethods->xSync(f.real, flags);
    },
    [](sqlite3_file* file, sqlite3_int64* size) {
        auto* real = unwrap(file); return real->pMethods->xFileSize(real, size);
    },
    [](sqlite3_file* file, int lock) { auto* real = unwrap(file); return real->pMethods->xLock(real, lock); },
    [](sqlite3_file* file, int lock) { auto* real = unwrap(file); return real->pMethods->xUnlock(real, lock); },
    [](sqlite3_file* file, int* result) { auto* real = unwrap(file); return real->pMethods->xCheckReservedLock(real, result); },
    [](sqlite3_file* file, int operation, void* argument) {
        auto* real = unwrap(file); return real->pMethods->xFileControl(real, operation, argument);
    },
    [](sqlite3_file* file) { auto* real = unwrap(file); return real->pMethods->xSectorSize(real); },
    [](sqlite3_file* file) { auto* real = unwrap(file); return real->pMethods->xDeviceCharacteristics(real); },
    [](sqlite3_file* file, int page, int pageSize, int extend, void volatile** result) {
        auto* real = unwrap(file); return real->pMethods->xShmMap(real, page, pageSize, extend, result);
    },
    [](sqlite3_file* file, int offset, int count, int flags) {
        auto* real = unwrap(file); return real->pMethods->xShmLock(real, offset, count, flags);
    },
    [](sqlite3_file* file) { auto* real = unwrap(file); real->pMethods->xShmBarrier(real); },
    [](sqlite3_file* file, int remove) { auto* real = unwrap(file); return real->pMethods->xShmUnmap(real, remove); },
    nullptr, nullptr
};

SqliteFaultVfs& SqliteFaultVfs::owner(sqlite3_vfs* vfs) { return *static_cast<SqliteFaultVfs*>(vfs->pAppData); }
sqlite3_vfs* SqliteFaultVfs::underlying(sqlite3_vfs* vfs) { return owner(vfs).underlying_; }

bool SqliteFaultVfs::fail(Failure failure) {
    if (failure_ != failure) return false;
    failure_ = Failure::None;
    triggered_ = true;
    return true;
}

int SqliteFaultVfs::open(sqlite3_vfs* vfs, const char* name, sqlite3_file* file, int flags, int* outFlags) {
    auto* f = new (file) File{};
    f->owner = &owner(vfs);
    f->wal = (flags & SQLITE_OPEN_WAL) != 0;
    auto* lower = underlying(vfs);
    f->real = static_cast<sqlite3_file*>(sqlite3_malloc(lower->szOsFile));
    if (!f->real) return SQLITE_NOMEM;
    std::memset(f->real, 0, static_cast<std::size_t>(lower->szOsFile));
    const int result = lower->xOpen(lower, name, f->real, flags, outFlags);
    if (result != SQLITE_OK) {
        if (f->real->pMethods) (void)f->real->pMethods->xClose(f->real);
        sqlite3_free(f->real);
        f->real = nullptr;
        return result;
    }
    if (f->real->pMethods->iVersion < 2) {
        (void)f->real->pMethods->xClose(f->real);
        sqlite3_free(f->real);
        f->real = nullptr;
        return SQLITE_CANTOPEN;
    }
    f->base.pMethods = &File::methods;
    return SQLITE_OK;
}

SqliteFaultVfs::SqliteFaultVfs() : underlying_(sqlite3_vfs_find(nullptr)) {
    if (!underlying_) throw std::runtime_error("SQLite default VFS is unavailable");
    // VFS v1 足以委派目录和时间服务；文件接口 v2 保留真实 WAL 共享内存与锁。
    vfs_.iVersion = 1;
    vfs_.szOsFile = static_cast<int>(sizeof(File));
    vfs_.mxPathname = underlying_->mxPathname;
    vfs_.zName = "anof-test-wal-fault";
    vfs_.pAppData = this;
    vfs_.xOpen = open;
    vfs_.xDelete = [](sqlite3_vfs* v, const char* name, int sync) { auto* b = underlying(v); return b->xDelete(b, name, sync); };
    vfs_.xAccess = [](sqlite3_vfs* v, const char* name, int flags, int* result) { auto* b = underlying(v); return b->xAccess(b, name, flags, result); };
    vfs_.xFullPathname = [](sqlite3_vfs* v, const char* name, int size, char* result) { auto* b = underlying(v); return b->xFullPathname(b, name, size, result); };
    vfs_.xDlOpen = [](sqlite3_vfs* v, const char* name) { auto* b = underlying(v); return b->xDlOpen(b, name); };
    vfs_.xDlError = [](sqlite3_vfs* v, int size, char* message) { auto* b = underlying(v); b->xDlError(b, size, message); };
    vfs_.xDlSym = [](sqlite3_vfs* v, void* handle, const char* name) { auto* b = underlying(v); return b->xDlSym(b, handle, name); };
    vfs_.xDlClose = [](sqlite3_vfs* v, void* handle) { auto* b = underlying(v); b->xDlClose(b, handle); };
    vfs_.xRandomness = [](sqlite3_vfs* v, int size, char* result) { auto* b = underlying(v); return b->xRandomness(b, size, result); };
    vfs_.xSleep = [](sqlite3_vfs* v, int duration) { auto* b = underlying(v); return b->xSleep(b, duration); };
    vfs_.xCurrentTime = [](sqlite3_vfs* v, double* result) { auto* b = underlying(v); return b->xCurrentTime(b, result); };
    vfs_.xGetLastError = [](sqlite3_vfs* v, int size, char* message) { auto* b = underlying(v); return b->xGetLastError ? b->xGetLastError(b, size, message) : 0; };
    if (sqlite3_vfs_register(&vfs_, 1) != SQLITE_OK) throw std::runtime_error("Cannot register SQLite fault VFS");
}

SqliteFaultVfs::~SqliteFaultVfs() {
    (void)sqlite3_vfs_register(underlying_, 1);
    (void)sqlite3_vfs_unregister(&vfs_);
}

}  // namespace anof::infrastructure::sqlite::testing
