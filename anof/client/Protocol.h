#pragma once
#include <anof/client/HttpTransport.h>
namespace anof::client {
enum class Operation { Register,
                       Login,
                       Logout,
                       Me,
                       CreateInvite,
                       ListInvites,
                       RevokeInvite,
                       Spaces,
                       Children,
                       Stat,
                       Mkdir,
                       Rename,
                       ChangePassword,
                       ListUsers,
                       GetUser,
                       SetUserEnabled,
                       GetQuota,
                       SetQuotaLimit };
using Payload = std::variant<Done, User, CurrentUser, Login, CreatedInvite, Page<Invite>, std::vector<Space>, Page<Node>, Node, AdminUser, Page<User>, ManagedQuota>;
ClientResult<Reply<Payload>> decodeResponse(Operation operation, const HttpResponse& response, unsigned pageLimit = 200);
ClientResult<std::string> pageQuery(const PageRequest& page);
}  // namespace anof::client
