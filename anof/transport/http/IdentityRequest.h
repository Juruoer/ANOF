#pragma once

#include <anof/application/identity/IdentityCommands.h>
#include <anof/transport/http/JsonResponse.h>
#include <muduox/net/http/HttpHeaders.h>
#include <muduox/net/http/HttpRequestMetadata.h>

#include <optional>
#include <string_view>
#include <variant>

namespace anof::http {
template <typename T>
using Parsed = std::variant<T, ProtocolError>;

// 输入是同步借用；成功命令自有数据，密码及邀请码只能移动。
Parsed<RegisterCommand> parseRegisterJson(std::string_view body, AuthenticationSource source);
Parsed<LoginCommand> parseLoginJson(std::string_view body, AuthenticationSource source);
Parsed<CreateInviteCommand> parseCreateInviteJson(std::string_view body);
Parsed<GetNodeCommand> parseGetNode(std::string_view id);
Parsed<RevokeInviteCommand> parseRevokeInvite(std::string_view id);

std::optional<ProtocolError> validateAnonymousCredentials(const muduox::net::HttpHeaders& headers);
// 选择阶段检查定界；complete 阶段仍须拒绝实际正文和 trailer，并关闭连接。
std::optional<ProtocolError> validateBodylessHeaders(const muduox::net::HttpHeaders& headers);
std::optional<ProtocolError> validateBodylessContent(std::string_view body, const muduox::net::HttpHeaders& trailers);
std::optional<ProtocolError> validateNoQuery(std::string_view query);
// 只接收可信 metadata，接口没有转发头输入；返回值不借用 metadata。
Parsed<AuthenticationSource> readAuthenticationSource(const muduox::net::HttpRequestMetadata& metadata);
}  // namespace anof::http
