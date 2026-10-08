#pragma once
#include <boost/test/unit_test.hpp>
#include <anof/transport/http/HttpApi.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <stdexcept>

namespace anof::http::testing {
using namespace muduox::net;
class Identity final : public RequestIdentity {
public:
    RequestId forExchange(std::uint64_t connection, std::uint64_t request) const override {
        char value[37];
        std::snprintf(value, sizeof value, "%08x-0000-4000-8000-%012llx", static_cast<unsigned>(connection), static_cast<unsigned long long>(request));
        return RequestId::parse(value).value();
    }
};
class Codec final : public SessionTokenCodec {
public:
    mutable int calls = 0;
    Result<SessionDigest> digestBearer(std::string_view token) const override {
        ++calls;
        if (token != "test-token") return ErrorCode::Unauthenticated;
        return SessionDigest(SessionDigest::Bytes{});
    }
};
struct Client {
    int fd;
    std::string bytes;
    bool eof = false;
    explicit Client(const InetAddress& address) : fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        if (fd < 0) throw std::runtime_error("socket failed");
        if (::connect(fd, address.getSockAddr(), sizeof(sockaddr_in)) != 0) {
            ::close(fd);
            throw std::runtime_error("connect failed");
        }
    }
    ~Client() { ::close(fd); }
    void send(const std::string& text) {
        const auto sent = ::send(fd, text.data(), text.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        BOOST_REQUIRE_EQUAL(sent, static_cast<ssize_t>(text.size()));
    }
    void read() {
        char buffer[4096];
        for (;;) {
            const auto n = ::recv(fd, buffer, sizeof buffer, MSG_DONTWAIT);
            if (n > 0) {
                bytes.append(buffer, static_cast<std::size_t>(n));
                if (bytes.size() > 256 * 1024) throw std::runtime_error("test response limit exceeded");
            } else if (n == 0 || (n < 0 && errno == ECONNRESET)) { eof = true; return; }
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            else throw std::runtime_error("recv failed");
        }
    }
};

}  // namespace anof::http::testing
