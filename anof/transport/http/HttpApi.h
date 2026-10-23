#pragma once
#include <anof/application/ports/DriveDispatcher.h>
#include <anof/application/ports/TransferDispatcher.h>

#include <anof/application/ports/SessionTokenCodec.h>
#include <anof/application/ports/IdentityDispatcher.h>
#include <anof/transport/http/RequestTrace.h>
#include <anof/application/drive/CreateDirectory.h>
#include <muduox/net/http/HttpServer.h>

namespace anof::http {

using CreateDirectoryStream = std::function<muduox::net::HttpStreamCallbacks(
    RequestContext, const muduox::net::HttpRequest&, const std::shared_ptr<muduox::net::HttpResponseWriter>&)>;

struct HttpApiOptions {
    std::size_t maxInFlight = 128;
    // 允许收紧期限供部署和测试使用，不允许扩大或禁用契约期限。
    double bodySeconds = 15;
    double responseSeconds = 30;
    double patchBodySeconds = 300;
    double patchResponseSeconds = 60;
    double contentResponseSeconds = 21600;
};

class HttpApi {
public:
    // ready 和 stream 必填；stream 收到自有身份上下文，但请求仍为借用。
    // 注入依赖及回调捕获资源须活过 server 的 HTTP 清理和全部外部任务完成。
    HttpApi(const SessionTokenCodec& codec, const RequestIdentity& identity,
            std::function<bool()> ready, CreateDirectoryStream stream, IdentityDispatcher& identityDispatcher, TransferDispatcher& transferDispatcher, DriveDispatcher& driveDispatcher,
            HttpObservation observation = {}, HttpApiOptions options = {});
    HttpApi(const HttpApi&) = delete;
    HttpApi& operator=(const HttpApi&) = delete;
    // 所属 loop、server.start 之前仅调用一次。不启动或停止 server，也不退出 loop。
    void configure(muduox::net::HttpServer& server);

private:
    IdentityDispatcher& identityDispatcher_;
    TransferDispatcher& transferDispatcher_;
    DriveDispatcher& driveDispatcher_;
    muduox::net::HttpRequestSelector selector_;
    RequestTrace trace_;
    bool configured_ = false;
};

}  // namespace anof::http
