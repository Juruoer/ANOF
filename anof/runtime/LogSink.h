#pragma once
#include <anof/core/Id.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string_view>
namespace anof::runtime {
enum class LogEvent { Access,
                      Ready,
                      Stopping,
                      Stopped,
                      Failure,
                      Library,
                      Dropped,
                      AuditRetention };
enum class LogLevel { Info,
                      Warning,
                      Error };
// route/code/outcome只能来自静态映射，禁止原请求、头、路径、正文和异常字符串。
struct LogRecord {
    LogEvent event = LogEvent::Failure;
    LogLevel level = LogLevel::Info;
    std::optional<RequestId> requestId;
    std::optional<OperationId> operationId;
    std::string_view route = "unmatched", code = "OK", outcome = "confirmed", stage = "runtime";
    unsigned status = 0, port = 0;
    std::uint64_t durationUs = 0, responseBytesSubmitted = 0;
};
struct LogSnapshot {
    std::uint64_t accepted, dropped, written, writeFailures;
};
class LogSink {
public:
    explicit LogSink(int output = 1, int errors = 2);
    ~LogSink();
    bool tryEmit(const LogRecord&) noexcept;
    void stop();  // flush上限2秒；等待消费者真实退出，无detach。
    LogSnapshot snapshot() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace anof::runtime
