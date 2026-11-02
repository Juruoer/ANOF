#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace anof::http::internal {
// 固定容量避免扩容遗留正文副本；clear/consume 清零仍由本对象拥有的字节。
class ExchangeBuffer {
public:
    explicit ExchangeBuffer(std::size_t capacity) : bytes_(capacity ? new char[capacity] : nullptr), capacity_(capacity) {}
    ~ExchangeBuffer() { clear(); }
    ExchangeBuffer(const ExchangeBuffer&) = delete;
    ExchangeBuffer& operator=(const ExchangeBuffer&) = delete;
    bool append(std::string_view text) {
        if (text.size() > capacity_ - size_) return false;
        if (!text.empty()) std::memcpy(bytes_.get() + size_, text.data(), text.size());
        size_ += text.size();
        return true;
    }
    std::string_view view() const noexcept { return size_ == offset_ ? std::string_view{} : std::string_view(bytes_.get() + offset_, size_ - offset_); }
    std::string take(std::size_t maximum) {
        if (maximum == 0) throw std::invalid_argument("Zero response chunk limit");
        const auto count = std::min(view().size(), maximum);
        std::string chunk(view().substr(0, count));
        erase(offset_, count);
        offset_ += count;
        return chunk;
    }
    void clear() noexcept {
        erase(offset_, size_ - offset_);
        offset_ = size_ = 0;
    }

private:
    void erase(std::size_t start, std::size_t count) noexcept {
        volatile char* data = bytes_.get();
        for (std::size_t i = start; i < start + count; ++i) data[i] = 0;
    }
    std::unique_ptr<char[]> bytes_;
    std::size_t capacity_;
    std::size_t size_ = 0;
    std::size_t offset_ = 0;
};
}  // namespace anof::http::internal
