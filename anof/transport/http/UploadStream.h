#pragma once
#include <anof/application/ports/TransferDispatcher.h>
#include <muduox/net/http/HttpResponseWriter.h>
namespace anof::http {
// 参数自有；dispatcher 与其 owner loop 必须活过 HTTP 清理和在途任务退休。
muduox::net::HttpStreamCallback makeUploadPatchStream(UploadDispatcher&, RequestContext, PreparePatchCommand);
}  // namespace anof::http
