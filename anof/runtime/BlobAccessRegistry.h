#pragma once
#include <anof/core/Id.h>
#include <map>
#include <memory>
namespace muduox::net { class EventLoop; }
namespace anof::runtime {
// 只由owner loop使用。仅活动资格占内存；DELETING持久状态由metadata复验。
class BlobAccessRegistry {
public:
    class Lease {
    public:
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
    private:
        friend class BlobAccessRegistry;
        Lease(BlobAccessRegistry& owner, BlobId id, bool write) : owner_(owner), id_(id), write_(write) {}
        BlobAccessRegistry& owner_;
        BlobId id_;
        bool write_;
        bool active_ = false;
    };
    explicit BlobAccessRegistry(muduox::net::EventLoop& loop) : loop_(loop) {}
    ~BlobAccessRegistry();
    BlobAccessRegistry(const BlobAccessRegistry&) = delete;
    BlobAccessRegistry& operator=(const BlobAccessRegistry&) = delete;
    std::unique_ptr<Lease> tryAcquireRead(BlobId);
    std::unique_ptr<Lease> tryAcquireDelete(BlobId);
    bool pinned(BlobId) const;
private:
    struct Entry { std::size_t readers = 0; bool deleting = false; };
    muduox::net::EventLoop& loop_;
    std::map<BlobId, Entry> entries_;
};
}
