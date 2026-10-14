#pragma once

#include <anof/application/ports/IdentityDispatcher.h>
#include <anof/transport/http/IdentityRequest.h>
#include <anof/transport/http/internal/AsyncRequestState.h>

#include <type_traits>
#include <utility>

namespace anof::http {
// parser 接收同步借用正文，返回 Parsed<Command>；须按值捕获自有路由/来源值。
// formatter 接收 Value&& 和 RequestId（void 仅 RequestId），返回 Result<JsonResponse>。
// mapError 仅转换已接纳任务的业务错误；同步提交拒绝沿用公共映射。
// 依赖和 ready 的引用资源须覆盖全部任务完成及 HTTP 清理；不依赖具体 executor。
template <typename Command, typename Value, typename Context, typename Parser, typename Formatter>
muduox::net::HttpStreamCallback makeIdentityStream(IdentityDispatcher& dispatcher, Context context,
    RequestBody bodyKind, Parser parser, Formatter formatter, std::function<bool()> ready, ProtocolError (*mapError)(ErrorCode) = protocolError) {
    static_assert(std::is_same_v<Context, RequestId> || std::is_same_v<Context, RequestContext>);
    if (!ready || !mapError) throw std::invalid_argument("Identity stream requires readiness callback and error mapper");
    const RequestId id = [&] {
        if constexpr (std::is_same_v<Context, RequestId>) return context;
        else return context.requestId;
    }();
    return [&dispatcher, context, id, bodyKind, parser = std::move(parser), formatter = std::move(formatter), ready = std::move(ready), mapError]
        (const muduox::net::HttpRequest&, const std::shared_ptr<muduox::net::HttpResponseWriter>& writer) {
        using State = internal::AsyncRequestState;
        auto requestState = std::make_shared<State>(id, bodyKind, writer, ready);
        return requestState->callbacks([&dispatcher, context, id, parser, formatter, mapError](std::string_view body, std::weak_ptr<State> weak) -> State::Submission {
            auto command = parser(body);
            if (const auto* error = std::get_if<ProtocolError>(&command)) return *error;
            Completion<Value> completion = [weak, id, formatter, mapError](TaskCompletion<Value> completed) {
                const auto state = weak.lock();
                if (!state || !state->beginCompletion(completed.taskId)) return;
                try {
                    auto* result = std::get_if<Result<Value>>(&completed.outcome);
                    if (!result) { state->fail(ProtocolError::TemporarilyUnavailable); return; }
                    if (!*result) { state->fail(mapError(result->error().code)); return; }
                    if constexpr (std::is_void_v<Value>) state->respond(formatter(id));
                    else state->respond(formatter(std::move(*result).value(), id));
                } catch (...) {
                    // 此回调在框架调用栈之外；转换/响应异常必须显式终止交换。
                    state->abort();
                }
            };
            auto submission = dispatcher.submit(context, std::get<Command>(std::move(command)), std::move(completion));
            if (auto* accepted = std::get_if<AcceptedTask>(&submission)) return std::move(*accepted);
            return std::get<SubmitError>(submission);
        });
    };
}
}  // namespace anof::http
