#pragma once
#include <anof/client/TransferTransport.h>
namespace anof::client {
struct TransferTimeouts {
    long patchMs = 365000, downloadMs = 21660000, noProgressMs = 120000;
};
class CurlHttpTransport final : public HttpTransport, public TransferTransport {
public:
    // 只能缩短契约期限；信任库与 TLS 验证不可由调用者关闭。
    explicit CurlHttpTransport(long connectTimeoutMs = 5000, long timeoutMs = 35000, TransferTimeouts transfers = {});
    ClientResult<HttpResponse> perform(const Origin&, const HttpRequest&, const std::atomic_bool* cancel = nullptr) override;
    ClientResult<HttpResponse> transfer(const Origin&, const TransferRequest&, const std::atomic_bool* cancel = nullptr) override;
private:
    ClientResult<HttpResponse> execute(const Origin&, const HttpRequest&, const std::atomic_bool*, const TransferRequest*);
    long connectTimeoutMs_, timeoutMs_;
    TransferTimeouts transfers_;
};
}  // namespace anof::client
