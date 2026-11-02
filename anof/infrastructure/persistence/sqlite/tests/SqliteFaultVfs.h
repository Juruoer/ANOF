#pragma once

#include <sqlite3.h>

namespace anof::infrastructure::sqlite::testing {

// 仅在单线程测试进程中临时替换默认 VFS；所有连接须在此对象之前销毁。
// 除指定的一次 WAL 故障外，I/O 均委派给本机真实 VFS。
class SqliteFaultVfs {
public:
    enum class Failure { None,
                         WalWrite,
                         WalSync };
    SqliteFaultVfs();
    ~SqliteFaultVfs();
    SqliteFaultVfs(const SqliteFaultVfs&) = delete;
    SqliteFaultVfs& operator=(const SqliteFaultVfs&) = delete;
    void arm(Failure failure) {
        failure_ = failure;
        triggered_ = false;
    }
    bool triggered() const { return triggered_; }

private:
    struct File;
    static SqliteFaultVfs& owner(sqlite3_vfs* vfs);
    static sqlite3_vfs* underlying(sqlite3_vfs* vfs);
    static int open(sqlite3_vfs* vfs, const char* name, sqlite3_file* file, int flags, int* outFlags);
    bool fail(Failure failure);
    sqlite3_vfs vfs_{};
    sqlite3_vfs* underlying_ = nullptr;
    Failure failure_ = Failure::None;
    bool triggered_ = false;
};

}  // namespace anof::infrastructure::sqlite::testing
