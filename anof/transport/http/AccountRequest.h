#pragma once

#include <anof/application/identity/AccountCommands.h>
#include <anof/transport/http/IdentityRequest.h>

namespace anof::http {
// 仅检查标签语法；保留完整绑定，不与路由或当前用户比较。
Parsed<AccountCondition> parseAccountCondition(const muduox::net::HttpHeaders& headers);
Parsed<GetUserCommand> parseGetUser(std::string_view id);
Parsed<QuotaScope> parseQuotaScope(std::string_view spaceId, std::optional<std::string_view> userId = std::nullopt);
// 输入同步借用，返回命令自有数据；密码只移动。expected 可以是任意合法资源条件。
Parsed<UpdateDisplayNameCommand> parseUpdateDisplayNameJson(std::string_view body, AccountCondition expected);
Parsed<ChangePasswordCommand> parseChangePasswordJson(std::string_view body, AccountCondition expected, AuthenticationSource source);
Parsed<SetUserEnabledCommand> parseSetUserEnabledJson(std::string_view body, UserId id, AccountCondition expected);
Parsed<SetQuotaLimitCommand> parseSetQuotaLimitJson(std::string_view body, QuotaScope scope, AccountCondition expected);
}  // namespace anof::http
