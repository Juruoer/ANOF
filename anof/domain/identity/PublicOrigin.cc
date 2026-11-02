#include <anof/domain/identity/PublicOrigin.h>
#include <arpa/inet.h>
#include <algorithm>
#include <charconv>
#include <array>

namespace anof {
bool isTrustedLoopbackProxy(std::string_view s) noexcept { return s == "127.0.0.1" || s == "::1"; }
Result<PublicOrigin> PublicOrigin::parse(std::string_view input) {
    if (input.size() > 256 || input.substr(0, 8) != "https://") return ErrorCode::InvalidArgument;
    std::string authority(input.substr(8));
    if (authority.empty()) return ErrorCode::InvalidArgument;
    for (unsigned char c : authority)
        if (c <= 0x20 || c >= 0x7f || c == '/' || c == '?' || c == '#' || c == '@' || c == '%' || c == '\\')
            return ErrorCode::InvalidArgument;
    std::string host, port;
    bool hasPort = false;
    if (authority.front() == '[') {
        auto end = authority.find(']');
        if (end == std::string::npos) return ErrorCode::InvalidArgument;
        auto literal = authority.substr(1, end - 1);
        in6_addr address{}; std::array<char, INET6_ADDRSTRLEN> text{};
        if (inet_pton(AF_INET6, literal.c_str(), &address) != 1 ||
            !inet_ntop(AF_INET6, &address, text.data(), text.size())) return ErrorCode::InvalidArgument;
        host = "[" + std::string(text.data()) + "]";
        if (end + 1 < authority.size()) {
            if (authority[end + 1] != ':') return ErrorCode::InvalidArgument;
            hasPort = true; port = authority.substr(end + 2);
        }
    } else {
        auto colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string::npos) { hasPort = true; port = authority.substr(colon + 1); }
        if (host.empty() || host.size() > 253 || host.back() == '.') return ErrorCode::InvalidArgument;
        std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) {
            return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        });
        if (std::all_of(host.begin(), host.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; })) {
            in_addr address{}; std::array<char, INET_ADDRSTRLEN> text{};
            if (inet_pton(AF_INET, host.c_str(), &address) != 1 ||
                !inet_ntop(AF_INET, &address, text.data(), text.size()) || host != text.data()) return ErrorCode::InvalidArgument;
        } else {
            // URL解析器会把末段数值的主机当成IPv4候选，拒绝八/十六进制和缩写歧义。
            auto last = host.substr(host.rfind('.') == std::string::npos ? 0 : host.rfind('.') + 1);
            if (std::all_of(last.begin(), last.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
                (last.size() > 2 && last.substr(0, 2) == "0x" &&
                 std::all_of(last.begin() + 2, last.end(), [](char c) {
                     return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                 }))) return ErrorCode::InvalidArgument;
            std::size_t start = 0;
            while (start < host.size()) {
                auto end = host.find('.', start); if (end == std::string::npos) end = host.size();
                if (end == start || end - start > 63 || host[start] == '-' || host[end-1] == '-') return ErrorCode::InvalidArgument;
                for (auto i = start; i < end; ++i)
                    if (!(host[i] >= 'a' && host[i] <= 'z') && !(host[i] >= '0' && host[i] <= '9') && host[i] != '-')
                        return ErrorCode::InvalidArgument;
                start = end + 1;
            }
        }
    }
    if (hasPort) {
        unsigned value = 0;
        auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
        if (port.empty() || (port.size() > 1 && port.front() == '0') || parsed.ec != std::errc{} ||
            parsed.ptr != port.data() + port.size() || value == 0 || value > 65535) return ErrorCode::InvalidArgument;
        if (value != 443) host += ":" + port;
    }
    return PublicOrigin("https://" + host);
}
}  // namespace anof
