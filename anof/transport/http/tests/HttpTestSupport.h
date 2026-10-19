#pragma once
#include <boost/test/unit_test.hpp>
#include <anof/transport/http/HttpApi.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <stdexcept>

namespace anof::http::testing {
using namespace muduox::net;
class Identity final : public RequestIdentity {
public:
    RequestId forExchange(std::uint64_t connection, std::uint64_t request) const override {
        char value[37];
        std::snprintf(value, sizeof value, "%08x-0000-4000-8000-%012llx", static_cast<unsigned>(connection), static_cast<unsigned long long>(request));
        return RequestId::parse(value).value();
    }
};
class Codec final : public SessionTokenCodec {
public:
    mutable int calls = 0;
    Result<SessionDigest> digestBearer(std::string_view token) const override {
        ++calls;
        if (token != "test-token") return ErrorCode::Unauthenticated;
        return SessionDigest(SessionDigest::Bytes{});
    }
};
// 现有 mkdir/health 不应提交身份任务；误调用立即使测试失败。
class UnexpectedIdentityDispatcher final : public IdentityDispatcher {
public:
    TaskSubmission submit(RequestId, RegisterCommand, Completion<UserProfile>) override {
        BOOST_ERROR("Unexpected identity submission: RegisterCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestId, LoginCommand, Completion<LoginResult>) override {
        BOOST_ERROR("Unexpected identity submission: LoginCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, LogoutCommand, Completion<void>) override {
        BOOST_ERROR("Unexpected identity submission: LogoutCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, GetMeCommand, Completion<UserProfile>) override {
        BOOST_ERROR("Unexpected identity submission: GetMeCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, CreateInviteCommand, Completion<CreatedInvite>) override {
        BOOST_ERROR("Unexpected identity submission: CreateInviteCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, ListInvitesCommand, Completion<Page<InviteInfo, InviteCursor>>) override {
        BOOST_ERROR("Unexpected identity submission: ListInvitesCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, RevokeInviteCommand, Completion<void>) override {
        BOOST_ERROR("Unexpected identity submission: RevokeInviteCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, ListSpacesCommand, Completion<std::vector<SpaceView>>) override {
        BOOST_ERROR("Unexpected identity submission: ListSpacesCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, GetNodeCommand, Completion<NodeDetails>) override {
        BOOST_ERROR("Unexpected identity submission: GetNodeCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, ListChildrenCommand, Completion<Page<NodeDetails, ChildrenCursor>>) override {
        BOOST_ERROR("Unexpected identity submission: ListChildrenCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, UpdateDisplayNameCommand, Completion<UserProfile>) override {
        BOOST_ERROR("Unexpected identity submission: UpdateDisplayNameCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, ChangePasswordCommand, Completion<void>) override {
        BOOST_ERROR("Unexpected identity submission: ChangePasswordCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, ListUsersCommand, Completion<Page<UserProfile, UserCursor>>) override {
        BOOST_ERROR("Unexpected identity submission: ListUsersCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, GetUserCommand, Completion<AdminUserView>) override {
        BOOST_ERROR("Unexpected identity submission: GetUserCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, SetUserEnabledCommand, Completion<AdminUserView>) override {
        BOOST_ERROR("Unexpected identity submission: SetUserEnabledCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, GetQuotaCommand, Completion<ManagedQuota>) override {
        BOOST_ERROR("Unexpected identity submission: GetQuotaCommand");
        return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, SetQuotaLimitCommand, Completion<ManagedQuota>) override {
        BOOST_ERROR("Unexpected identity submission: SetQuotaLimitCommand");
        return SubmitError::InternalError;
    }
};
class UnexpectedTransferDispatcher final : public TransferDispatcher {
public:
    TaskSubmission submit(RequestContext, OpenDownloadCommand, Completion<OpenedDownload>) override {
        BOOST_ERROR("Unexpected download submission"); return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, CreateUploadCommand, Completion<UploadCreated>) override {
        BOOST_ERROR("Unexpected upload submission: CreateUploadCommand"); return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, ListUploadsCommand, Completion<Page<UploadView, UploadCursor>>) override {
        BOOST_ERROR("Unexpected upload submission: ListUploadsCommand"); return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, GetUploadCommand, Completion<UploadView>) override {
        BOOST_ERROR("Unexpected upload submission: GetUploadCommand"); return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, CancelUploadCommand, Completion<void>) override {
        BOOST_ERROR("Unexpected upload submission: CancelUploadCommand"); return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, CompleteUploadCommand, Completion<UploadCompletionAccepted>) override {
        BOOST_ERROR("Unexpected upload submission: CompleteUploadCommand"); return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, PreparePatchCommand, Completion<PatchPreparation>) override {
        BOOST_ERROR("Unexpected upload submission: PreparePatchCommand"); return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, GetStorageCommand, Completion<StorageView>) override {
        BOOST_ERROR("Unexpected upload submission: GetStorageCommand"); return SubmitError::InternalError;
    }
    TaskSubmission submit(RequestContext, SetStorageBudgetCommand, Completion<StorageView>) override {
        BOOST_ERROR("Unexpected upload submission: SetStorageBudgetCommand"); return SubmitError::InternalError;
    }
};
struct Client {
    int fd;
    std::string bytes;
    bool eof = false;
    const std::size_t responseLimit;
    explicit Client(const InetAddress& address, std::size_t limit = 256 * 1024)
        : fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)), responseLimit(limit) {
        if (fd < 0) throw std::runtime_error("socket failed");
        if (::connect(fd, address.getSockAddr(), sizeof(sockaddr_in)) != 0) {
            ::close(fd);
            throw std::runtime_error("connect failed");
        }
    }
    ~Client() { ::close(fd); }
    void send(const std::string& text) {
        const auto sent = ::send(fd, text.data(), text.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        BOOST_REQUIRE_EQUAL(sent, static_cast<ssize_t>(text.size()));
    }
    void read() {
        char buffer[4096];
        for (;;) {
            const auto n = ::recv(fd, buffer, sizeof buffer, MSG_DONTWAIT);
            if (n > 0) {
                bytes.append(buffer, static_cast<std::size_t>(n));
                if (bytes.size() > responseLimit) throw std::runtime_error("test response limit exceeded");
            } else if (n == 0 || (n < 0 && errno == ECONNRESET)) { eof = true; return; }
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            else throw std::runtime_error("recv failed");
        }
    }
};

}  // namespace anof::http::testing
