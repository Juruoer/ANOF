#include <anof/runtime/BlobAccessRegistry.h>
#include <muduox/net/EventLoop.h>
#include <exception>
namespace anof::runtime {
BlobAccessRegistry::~BlobAccessRegistry() { loop_.assertInLoopThread(); if (!entries_.empty()) std::terminate(); }
BlobAccessRegistry::Lease::~Lease() {
    owner_.loop_.assertInLoopThread();
    if (!active_) return;
    auto it = owner_.entries_.find(id_);
    if (it == owner_.entries_.end()) std::terminate();
    if (write_) it->second.deleting = false; else --it->second.readers;
    if (!it->second.deleting && !it->second.readers) owner_.entries_.erase(it);
}
std::unique_ptr<BlobAccessRegistry::Lease> BlobAccessRegistry::tryAcquireRead(BlobId id) {
    loop_.assertInLoopThread();
    auto it = entries_.find(id);
    if (it != entries_.end() && it->second.deleting) return {};
    // 先分配，避免内存异常留下虚假资格。
    auto lease = std::unique_ptr<Lease>(new Lease(*this, id, false));
    ++entries_[id].readers;
    lease->active_ = true;
    return lease;
}
std::unique_ptr<BlobAccessRegistry::Lease> BlobAccessRegistry::tryAcquireDelete(BlobId id) {
    loop_.assertInLoopThread();
    if (entries_.count(id)) return {};
    auto lease = std::unique_ptr<Lease>(new Lease(*this, id, true));
    entries_[id].deleting = true;
    lease->active_ = true;
    return lease;
}
bool BlobAccessRegistry::pinned(BlobId id) const {
    loop_.assertInLoopThread(); auto it = entries_.find(id); return it != entries_.end() && it->second.readers;
}
}
