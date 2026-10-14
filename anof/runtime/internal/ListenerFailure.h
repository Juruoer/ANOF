#pragma once

#include <anof/core/Error.h>
#include <muduox/net/http/HttpServer.h>

namespace anof::runtime::internal {

// 监听失败不是 HTTP 业务响应；不向应用端口传播系统错误文本。
inline ErrorCode listenerFailure(const muduox::net::HttpServer::StartResult& result) {
    if (result.status != muduox::net::HttpServer::StartStatus::ListenFailed) return ErrorCode::InternalError;
    const auto code = result.error.code;
    if (result.error.stage == muduox::net::ListenerStage::Bind && code == std::errc::address_in_use)
        return ErrorCode::Conflict;
    if (code == std::errc::permission_denied || code == std::errc::operation_not_permitted)
        return ErrorCode::Forbidden;
    if (code == std::errc::too_many_files_open || code == std::errc::too_many_files_open_in_system ||
        code == std::errc::not_enough_memory || code == std::errc::no_buffer_space ||
        code == std::errc::resource_unavailable_try_again || code == std::errc::interrupted)
        return ErrorCode::Retryable;
    return ErrorCode::InternalError;
}

}  // namespace anof::runtime::internal
