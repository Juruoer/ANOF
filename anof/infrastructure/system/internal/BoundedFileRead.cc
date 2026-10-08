#include <anof/infrastructure/system/internal/BoundedFileRead.h>
#include <anof/infrastructure/system/internal/InstanceFiles.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>

namespace anof::infrastructure::system::detail {

Result<std::string> readBoundedFileWith(int fd, std::size_t limit, ErrorCode oversized, const FileReader& read) {
    if (limit > 65536 || !read) return ErrorCode::InvalidArgument;
    struct stat status{};
    if (::fstat(fd, &status) != 0) return filesystemError(errno);
    if (!S_ISREG(status.st_mode) || status.st_size < 0) return ErrorCode::IntegrityError;
    if (static_cast<std::uintmax_t>(status.st_size) > limit) return oversized;
    std::string text;
    std::array<char, 4096> buffer{};
    for (;;) {
        // 多探测一个字节以判断恰好达到上限后的 EOF，不依赖初始 st_size。
        const auto requested = std::min(buffer.size(), limit - text.size() + 1);
        const auto received = read(fd, buffer.data(), requested);
        if (received < 0) {
            if (errno == EINTR) continue;
            return filesystemError(errno);
        }
        if (static_cast<std::uintmax_t>(received) > requested) return ErrorCode::InternalError;
        if (received == 0) break;
        const auto count = static_cast<std::size_t>(received);
        if (count > limit - text.size()) return oversized;
        text.append(buffer.data(), count);
    }
    if (::fstat(fd, &status) != 0) return filesystemError(errno);
    if (status.st_size < 0) return ErrorCode::IntegrityError;
    if (static_cast<std::uintmax_t>(status.st_size) > limit) return oversized;
    return text;
}

Result<std::string> readBoundedFile(int fd, std::size_t limit, ErrorCode oversized) {
    return readBoundedFileWith(fd, limit, oversized, ::read);
}

}  // namespace anof::infrastructure::system::detail
