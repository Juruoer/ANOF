#pragma once

#include <anof/core/Id.h>
#include <anof/core/Quantity.h>
#include <anof/domain/identity/IdentityValues.h>
#include <anof/domain/identity/Secrets.h>

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace anof::client {
enum class ErrorKind { InvalidArgument,
                       AuthenticationRequired,
                       Application,
                       Transport,
                       Protocol,
                       LocalState,
                       OutcomeUnknown,
                       Interrupted };
enum class RemoteOutcome { NotSent,
                           Rejected,
                           ConfirmedSuccess,
                           Unknown };
enum class LocalState { Unchanged,
                        CredentialSaved,
                        CredentialCleared,
                        TransferSaved,
                        DownloadPartial,
                        FilePublished,
                        Unknown };
struct ClientError {
    ErrorKind kind;
    std::string code;
    std::optional<int> httpStatus;
    std::optional<RequestId> requestId;
    RemoteOutcome remote = RemoteOutcome::NotSent;
    LocalState local = LocalState::Unchanged;
    std::optional<std::string> profileName;  // 仅含已验证的本地名称，用于定位损坏的列表项。
    std::optional<std::string> transferId;
    int exitCode() const noexcept;
};
ClientError failure(ErrorKind kind, std::string code = {});
const char* errorMessage(ErrorKind kind) noexcept;
template <class T>
class ClientResult {
public:
    ClientResult(T value) : value_(std::move(value)) {}
    ClientResult(ClientError error) : value_(std::move(error)) {}
    explicit operator bool() const noexcept { return std::holds_alternative<T>(value_); }
    T& value() & { return std::get<T>(value_); }
    const T& value() const& { return std::get<T>(value_); }
    T&& value() && { return std::get<T>(std::move(value_)); }
    ClientError& error() { return std::get<ClientError>(value_); }
    const ClientError& error() const { return std::get<ClientError>(value_); }

private:
    std::variant<T, ClientError> value_;
};
using Done = std::monostate;

// 固定容量堆缓冲；追加不重新分配，自有正文（包括秘密）销毁时清零。
class SecureBuffer {
public:
    SecureBuffer() = default;
    explicit SecureBuffer(std::size_t capacity);
    ~SecureBuffer();
    SecureBuffer(SecureBuffer&&) noexcept;
    SecureBuffer& operator=(SecureBuffer&&) noexcept;
    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;
    bool append(std::string_view text) noexcept;
    std::string_view view() const noexcept;
    void clear() noexcept;

private:
    std::unique_ptr<char[]> bytes_;
    std::size_t capacity_ = 0, size_ = 0;
};
struct ResponseMeta {
    int httpStatus;
    RequestId requestId;
    std::optional<std::string> etag;
};
template <class T>
struct Reply {
    T data;
    ResponseMeta meta;
};
struct User {
    UserId id;
    LoginName login;
    DisplayName displayName;
    enum class Role { Admin,
                      Member } role;
    bool enabled;
    Revision revision;
    std::string createdAt, updatedAt;
};
struct CurrentUser {
    User user;
    InstanceId instanceId;
};
struct AdminUser {
    User user;
    SpaceId privateSpaceId;
};
struct QuotaTarget {
    SpaceId spaceId;
    std::optional<UserId> userId;
};
// 保留完整条件标签；合法但错配的绑定由服务端在授权后处理。
class ConditionTag {
public:
    static ClientResult<ConditionTag> parse(std::string_view);
    const std::string& value() const noexcept { return value_; }

private:
    explicit ConditionTag(std::string value) : value_(std::move(value)) {}
    std::string value_;
};
struct Login {
    User user;
    std::string expiresAt;
    SecretToken accessToken;
};
struct Invite {
    InviteId id;
    UserId createdBy;
    std::string createdAt, expiresAt;
    InviteUseLimit maxUses;
    InviteUseCount usedCount;
    std::optional<std::string> revokedAt;
};
struct CreatedInvite {
    Invite invite;
    SecretToken token;
};
struct Amount {
    ByteCount bytes;
    NodeCount nodes;
};
struct Quota {
    Amount limit, used, reserved;
    Revision revision;
};
struct ManagedQuota {
    QuotaTarget target;
    Quota quota;
};
struct Space {
    SpaceId id;
    enum class Kind { Private,
                      Public } kind;
    std::optional<UserId> ownerId;
    NodeId rootNodeId;
    Quota quota;
    std::optional<Quota> myContributionQuota;
};
struct FileContent {
    ByteCount size;
    std::string sha256;
};
struct Node {
    NodeId id;
    SpaceId spaceId;
    std::optional<NodeId> parentId;
    std::string name;
    UserId createdBy;
    std::string createdAt, updatedAt, etag;
    std::optional<FileContent> file;
};
template <class T>
struct Page {
    std::vector<T> items;
    std::optional<std::string> nextCursor;
};
struct PageRequest {
    unsigned limit = 100;
    std::optional<std::string> cursor;
};
}  // namespace anof::client
