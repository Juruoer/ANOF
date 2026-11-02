#pragma once
#include <anof/client/ClientTypes.h>

#include <atomic>
#include <map>
namespace anof::client {
inline constexpr std::size_t kRequestLimit = 64 * 1024, kResponseLimit = 512 * 1024, kHeaderLimit = 64 * 1024;
class Origin {
public:
    static ClientResult<Origin> parse(std::string_view text, bool developmentHttp);
    const std::string& value() const noexcept { return value_; }
    bool developmentHttp() const noexcept { return developmentHttp_; }

private:
    Origin(std::string value, bool developmentHttp) : value_(std::move(value)), developmentHttp_(developmentHttp) {}
    std::string value_;
    bool developmentHttp_;
};
enum class Method { Get,
                    Head,
                    Post,
                    Delete,
                    Patch,
                    Put };
struct HttpRequest {
    Method method;
    std::string path;
    SecureBuffer body;
    const SecretToken* bearer = nullptr;  // 仅在同步 perform 生命周期中借用。
    std::optional<ConditionTag> ifMatch = {};
    bool writes() const noexcept { return method != Method::Get && method != Method::Head; }
};
struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;  // 小写名称；只保留最终响应头。
    SecureBuffer body{ kResponseLimit };
};
class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual ClientResult<HttpResponse> perform(const Origin&, const HttpRequest&, const std::atomic_bool* cancel = nullptr) = 0;
};
}  // namespace anof::client
