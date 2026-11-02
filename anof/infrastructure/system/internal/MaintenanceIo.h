#pragma once
#include <sys/types.h>

#include <memory>
namespace anof::infrastructure::system::maintenance {
// 同步适配器的内部故障接缝；生产默认直接调用 POSIX。
class Io {
public:
    virtual ~Io() = default;
    virtual ssize_t read(int, void*, std::size_t);
    virtual ssize_t write(int, const void*, std::size_t);
    virtual int sync(int);
    virtual int truncate(int, off_t);
    virtual int rename(int, const char*, int, const char*);
};
}  // namespace anof::infrastructure::system::maintenance
