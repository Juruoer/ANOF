#pragma once
#include <anof/client/HttpTransport.h>

#include <functional>
namespace anof::client {
// 同步借用回调；每次 source/sink 至多64KiB，headers在任何正文交付前调用。
struct TransferRequest {
    Method method;
    std::string path;
    const SecretToken* bearer = nullptr;
    std::map<std::string, std::string> headers;
    std::int64_t length = 0;
    std::function<ClientResult<std::size_t>(char*, std::size_t)> source;
    std::function<ClientResult<Done>(const HttpResponse&)> validate;
    std::function<ClientResult<Done>(std::string_view)> sink;
};
class TransferTransport {
public:
    virtual ~TransferTransport() = default;
    virtual ClientResult<HttpResponse> transfer(const Origin&, const TransferRequest&,
                                                const std::atomic_bool* = nullptr) = 0;
};
}  // namespace anof::client
