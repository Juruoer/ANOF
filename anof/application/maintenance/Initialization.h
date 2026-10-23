#pragma once

#include <anof/application/ports/InstancePreparation.h>
#include <anof/domain/identity/User.h>
#include <anof/domain/instance/InstanceSettings.h>
#include <variant>
#include <anof/domain/drive/NodeAccounting.h>

namespace anof {
enum class InitError {
    InvalidInput, NotFound, AlreadyExists, Busy, IncompleteInitialization, UnsupportedFormat,
    IntegrityError, StorageFull, CommitOutcomeUnknown, PublicationOutcomeUnknown, IoError, InternalError
};
enum class InitStage {
    Validate, HashPassword, CreateDirectory, AcquireLock, WritePending, WriteConfiguration,
    CreateDatabase, Bootstrap, Verify, PublishFormat, ClearPending
};
struct InitFailure {
    InitError code;
    InitStage stage;
};

// 不复用业务错误枚举：数据库提交未知和文件发布未知是不同恢复事实。
template <typename T>
class [[nodiscard]] InitResult {
public:
    InitResult(T value) : state_(std::move(value)) {}
    InitResult(InitFailure error) : state_(error) {}
    bool hasValue() const noexcept { return std::holds_alternative<T>(state_); }
    explicit operator bool() const noexcept { return hasValue(); }
    T& value() & { return std::get<T>(state_); }
    const T& value() const& { return std::get<T>(state_); }
    T&& value() && { return std::get<T>(std::move(state_)); }
    const InitFailure& error() const { return std::get<InitFailure>(state_); }
private:
    std::variant<T, InitFailure> state_;
};
template <>
class [[nodiscard]] InitResult<void> {
public:
    InitResult() = default;
    InitResult(InitFailure error) : state_(error) {}
    bool hasValue() const noexcept { return std::holds_alternative<std::monostate>(state_); }
    explicit operator bool() const noexcept { return hasValue(); }
    void value() const { (void)std::get<std::monostate>(state_); }
    const InitFailure& error() const { return std::get<InitFailure>(state_); }
private:
    std::variant<std::monostate, InitFailure> state_;
};

struct InitializationIdentity {
    InstanceId instanceId;
    OperationId operationId;
    static constexpr std::int64_t schemaVersion = 4;
    static constexpr std::int64_t formatVersion = 4;
};
struct InitReceipt {
    InitializationIdentity identity;
    UserId adminUserId;
    SpaceId publicSpaceId;
    SpaceId privateSpaceId;
};
struct InitialQuotas {
    QuotaDelta privateSpace;
    QuotaDelta publicSpace;
    QuotaDelta contribution;
};
struct InitOptions {
    std::filesystem::path absoluteRoot;
    LoginName adminLogin;
    DisplayName adminDisplayName;
    InitialQuotas quotas;
    UserLimit maxUsers;
    InstanceConfiguration configuration;
};
struct BootstrapSeed {
    InitializationIdentity identity;
    UserId adminUserId;
    LoginName adminLogin;
    DisplayName adminDisplayName;
    EncodedPasswordHash encodedPassword;
    SpaceId publicSpaceId;
    SpaceId privateSpaceId;
    NodeId publicRootId;
    NodeId privateRootId;
    InitialQuotas quotas;
    UserLimit maxUsers;
    RequestId requestId;
    UtcTime createdAt;
};

Result<void> validateInitialQuotas(const InitialQuotas& quotas);
Result<NodeAccounting> initialNodeAccounting(const BootstrapSeed&);
Result<void> validateBootstrapSeed(const BootstrapSeed& seed);
}  // namespace anof
