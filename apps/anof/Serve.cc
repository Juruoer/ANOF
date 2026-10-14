#include <anof/runtime/internal/ShutdownTrace.h>
#include <anof/runtime/InstancePasswordPolicy.h>
#include <apps/anof/Serve.h>
#include <apps/anof/ShutdownSignals.h>
#include <anof/application/maintenance/PrepareInstance.h>
#include <anof/infrastructure/config/TomlInstanceConfiguration.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceMaintenance.h>
#include <anof/infrastructure/system/LinuxInstanceLock.h>
#include <anof/runtime/InstanceServer.h>
#include <muduox/net/EventLoop.h>

#include <cstdio>

namespace anof::app {
namespace {
const char* errorName(ErrorCode error) {
    switch (error) {
    case ErrorCode::InvalidArgument: return "INVALID_ARGUMENT";
    case ErrorCode::NotFound: return "NOT_FOUND";
    case ErrorCode::Conflict: return "CONFLICT";
    case ErrorCode::Forbidden: return "FORBIDDEN";
    case ErrorCode::Retryable: return "RETRYABLE";
    case ErrorCode::StorageFull: return "STORAGE_FULL";
    case ErrorCode::IntegrityError: return "INTEGRITY_ERROR";
    case ErrorCode::CommitOutcomeUnknown: return "COMMIT_OUTCOME_UNKNOWN";
    default: return "INTERNAL_ERROR";
    }
}
int failure(const char* stage, ErrorCode error, int code) {
    // 只输出固定阶段与稳定错误分类，不包含配置、路径、凭据或底层异常文本。
    std::fprintf(stderr, "ANOF_ERROR stage=%s code=%s\n", stage, errorName(error));
    return code;
}
}

int serve(const std::filesystem::path& root) {
    auto signalFactory = ShutdownSignals::create();
    if (!signalFactory) return failure("signals", signalFactory.error().code, 4);
    auto signals = std::move(signalFactory).value();
    auto locks = infrastructure::system::makeLinuxInstanceLockProvider();
    auto configuration = infrastructure::config::makeTomlInstanceConfigurationLoader();
    auto maintenance = infrastructure::sqlite::makeSqliteInstanceMaintenance({ runtime::instancePasswordPolicy().verification, {} });
    if (!locks || !configuration || !maintenance) return failure("adapters", ErrorCode::InternalError, 4);
    auto prepared = prepareInstance(root, *locks.value(), *configuration.value(), *maintenance.value());
    if (!prepared) return failure("prepare", prepared.error().code, 3);
    auto pendingSignal = signals->poll();
    if (!pendingSignal) return failure("signals", pendingSignal.error().code, 5);
    // 同步维护不可被信号强行打断；返回后先消费信号，避免已请求停止仍创建 worker。
    if (pendingSignal.value()) return 0;

    int exitCode = 0;
    {
        muduox::net::EventLoop loop;
        auto factory = runtime::makeInstanceServer(loop, std::move(prepared).value());
        if (!factory) return failure("composition", factory.error().code, 4);
        auto server = std::move(factory).value();
        bool signalStop = false;
        bool stopped = false;
        const auto consumeSignal = [&] {
            auto request = signals->poll();
            if (!request) {
                exitCode = failure("signals", request.error().code, 5);
                server->requestStop();
            } else if (request.value()) {
                if (!signalStop) {
                    runtime::internal::shutdownTrace("serve", "signal_consumed");
                    std::puts("ANOF_STOPPING");
                    std::fflush(stdout);
                }
                signalStop = true;
                server->requestStop();
            }
        };
        // timer 在 worker 前注册；信号仅触发停止，不直接 quit loop 或释放 lease。
        const auto timer = loop.runEvery(0.05, consumeSignal);
        auto started = server->start([&](Result<void> ready) {
            if (!ready) {
                if (!(signalStop && ready.error().code == ErrorCode::Retryable))
                    exitCode = failure("startup", ready.error().code, 4);
                return;
            }
            consumeSignal();
            if (server->ready()) {
                std::printf("ANOF_READY address=127.0.0.1 port=%u\n", static_cast<unsigned>(*server->port()));
                std::fflush(stdout);
            }
        }, [&] { runtime::internal::shutdownTrace("serve", "stop_callback_quit"); stopped = true; loop.quit(); });
        if (!started) {
            loop.cancel(timer);
            return failure("startup", started.error().code, 4);
        }
        // 只有完整服务停止回调才能退出；外部 grace 不限制正在运行的 worker I/O。
        if (!stopped) loop.loop();
        runtime::internal::shutdownTrace("serve", "loop_returned");
        loop.cancel(timer);
        if (server->state() != runtime::RuntimeState::Stopped) std::terminate();
        if (server->notificationExceptions() != 0) exitCode = failure("notifications", ErrorCode::InternalError, 5);
        runtime::internal::shutdownTrace("serve", "destroy_instance");
        server.reset();  // 回调栈外释放 HTTP、worker、crypto，最后释放实例锁。
    }
    std::puts("ANOF_STOPPED");
    std::fflush(stdout);
    return exitCode;
}
}  // namespace anof::app
