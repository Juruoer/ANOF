#pragma once
#include <anof/application/ports/TransferDispatcher.h>
#include <muduox/net/http/HttpResponseWriter.h>
namespace anof::http {
muduox::net::HttpStreamCallback makeDownloadStream(TransferDispatcher&, RequestContext, OpenDownloadCommand,
    muduox::net::HttpHeaders conditions, std::function<bool()> ready);
}  // namespace anof::http
