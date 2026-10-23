#pragma once
#include <anof/application/ports/DriveDispatcher.h>
#include <muduox/net/http/HttpRequestDecision.h>
namespace anof::http {
// 仅同步借用选择器输入；返回的流式回调只捕获自有命令/条件。
std::optional<muduox::net::HttpRequestDecision> driveDecision(
    const muduox::net::HttpRequest&, std::string_view pattern, std::optional<std::string_view> routeId,
    RequestContext, DriveDispatcher&, const std::function<bool()>& ready);
}  // namespace anof::http
