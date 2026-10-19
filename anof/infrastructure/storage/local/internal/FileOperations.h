#pragma once
#include <anof/infrastructure/storage/local/LocalTransferFileStore.h>
#include <sys/types.h>
namespace anof::infrastructure::storage::detail {
// 仅内部故障接缝；默认实现始终调用真实 POSIX 操作。
class FileOperations {
public:
    virtual ~FileOperations() = default;
    virtual ssize_t read(int, void*, std::size_t, off_t);
    virtual ssize_t write(int, const void*, std::size_t, off_t);
    virtual int sync(int, bool data);
    virtual int truncate(int, off_t);
    virtual int publish(int, const char*, int, const char*);
    virtual int remove(int, const char*);
};
FileResult<std::unique_ptr<TransferFileStore>> openLocalTransferFileStore(
    const std::filesystem::path&, std::shared_ptr<FileOperations>);
}  // namespace anof::infrastructure::storage::detail
