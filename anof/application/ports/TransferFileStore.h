#pragma once
#include <anof/core/Page.h>
#include <anof/domain/drive/Blob.h>
#include <anof/domain/transfer/Upload.h>
#include <memory>
#include <variant>

namespace anof {
enum class TransferFileError { NotFound, AlreadyExists, NoSpace, Transient, Integrity, UnknownEffect };
// UnknownEffect 包括 rename/unlink/同步结果不明；不能直接映射为已回滚。
template <typename T>
using FileResult = std::variant<T, TransferFileError>;
using FileStatus = FileResult<std::monostate>;
struct TemporaryFile { UploadId id; };
struct ObjectFile { BlobId id; };
using TransferFileKey = std::variant<TemporaryFile, ObjectFile>;
// 句柄由 unique_ptr 独占，不暴露 fd/path；只在有界 disk worker 使用，不跨并发任务共享。
class TransferFileHandle {
public:
    TransferFileHandle() = default;
    TransferFileHandle(const TransferFileHandle&) = delete;
    TransferFileHandle& operator=(const TransferFileHandle&) = delete;
    virtual ~TransferFileHandle() = default;
};
using OwnedTransferFile = std::unique_ptr<TransferFileHandle>;
class TransferHashState {
public:
    TransferHashState() = default;
    TransferHashState(const TransferHashState&) = delete;
    TransferHashState& operator=(const TransferHashState&) = delete;
    virtual ~TransferHashState() = default;
};
using OwnedTransferHash = std::unique_ptr<TransferHashState>;
struct TransferBlock {
    std::array<std::uint8_t, kTransferBlockBytes> bytes{};
    // 唯一缓冲所有权移交；有效内容为 [0,size)，构造输入必须检查 size<=capacity。
    std::size_t size = 0;
};
struct TransferFileStat { ByteCount size; };
struct DiskCapacity { ByteCount free; ByteCount volume; };
enum class InventoryArea { Uploads, Objects, Quarantine };
// 枚举路径不是 IO key；未知项原样保留，无法归属则拒绝启动。游标仅同一启动扫描使用。
struct InventoryEntry { std::string relativeName; std::optional<TransferFileKey> knownKey; ByteCount size; };
struct InventoryCursor { std::string opaque; };
class TransferFileStore {
public:
    virtual ~TransferFileStore() = default;
    // exclusive/no-follow；成功表示文件和父目录已同步。UnknownEffect 后先复核证据。
    virtual FileResult<OwnedTransferFile> create(UploadId) = 0;
    // 所有打开检查普通文件、单链接、UID、目录和文件身份；object 请求 writable 必须拒绝。
    virtual FileResult<OwnedTransferFile> open(const TransferFileKey&, bool writable) = 0;
    virtual FileResult<TransferFileStat> stat(TransferFileHandle&) = 0;
    // 单次<=64KiB，检查 offset+size/系统整数范围，内部处理 EINTR/短写；成功为整个块，失败允许存在未确认尾部。
    virtual FileStatus write(TransferFileHandle&, UploadOffset, const TransferBlock&) = 0;
    // 填调用者唯一缓冲，不另分配第二块；读满 count，提前 EOF 为 Integrity。
    virtual FileStatus read(TransferFileHandle&, UploadOffset, std::size_t count, TransferBlock&) = 0;
    virtual FileStatus syncData(TransferFileHandle&) = 0;
    virtual FileStatus truncateAndSync(TransferFileHandle&, UploadOffset) = 0;
    // 同文件系统 no-replace，成功前同步源/目标及新分片目录；已存在不能认领。
    virtual FileStatus publish(UploadId, BlobId) = 0;
    // 恢复已发生的 rename：要求源不存在、目标为安全对象，复核目录/文件身份，
    // 同步对象数据及 uploads、目标分片、objects 目录。可重复；不创建/搬运/覆盖/删除。
    // 源仍存在（含两者都有）或目标缺失为 Integrity；同步结果未知为 UnknownEffect。
    // 调用方须依据持久化意图先完整核对长度/SHA-256，并持有独占资格至同步和提交结束。
    virtual FileStatus syncPublication(UploadId, BlobId) = 0;
    // 已不存在也同步父目录；成功后 metadata 才能确认清理、释放 R。
    virtual FileStatus removeAndSync(const TransferFileKey&) = 0;
    virtual FileResult<OwnedTransferHash> beginHash() = 0;
    // 每次只消费一个块；调度器负责顺序完整扫描，不在内部阻塞扫描整个文件。
    virtual FileStatus updateHash(TransferHashState&, const TransferBlock&) = 0;
    virtual FileResult<Sha256> finishHash(OwnedTransferHash) = 0;
    virtual FileResult<Page<InventoryEntry, InventoryCursor>> inventory(InventoryArea, PageSize,
                                                                       std::optional<InventoryCursor>) = 0;
    virtual FileResult<DiskCapacity> capacity() = 0;
};
}  // namespace anof
