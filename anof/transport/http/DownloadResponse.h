#pragma once
#include <anof/application/transfer/TransferCommands.h>
#include <anof/transport/http/IdentityRequest.h>
namespace anof::http {
// 所有字段自有；只保留公开条件求值所需的有界头，不保留凭据。
Parsed<muduox::net::HttpHeaders> copyDownloadConditions(const muduox::net::HttpHeaders&);
struct DownloadResponse {
    muduox::net::HttpResponse headers{false};
    std::optional<std::size_t> length;
    std::uint64_t offset = 0;
    bool content = false;
};
// 仅在 dispatcher 已完成授权、open/stat 核对后调用。
Result<DownloadResponse> downloadResponse(const DownloadDescriptor&, const muduox::net::HttpHeaders&, bool head, RequestId);
}  // namespace anof::http
