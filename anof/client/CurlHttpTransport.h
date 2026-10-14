#pragma once
#include <anof/client/HttpTransport.h>
namespace anof::client {
class CurlHttpTransport final : public HttpTransport {
public:
    // 只能缩短契约期限；信任库与 TLS 验证不可由调用者关闭。
    explicit CurlHttpTransport(long connectTimeoutMs = 5000, long timeoutMs = 35000);
    ClientResult<HttpResponse> perform(const Origin&, const HttpRequest&, const std::atomic_bool* cancel = nullptr) override;
private:
    long connectTimeoutMs_, timeoutMs_;
};
}  // namespace anof::client
