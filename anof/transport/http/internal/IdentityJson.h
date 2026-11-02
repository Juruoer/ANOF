#pragma once
#include <anof/application/identity/IdentityCommands.h>

#include <nlohmann/json.hpp>

namespace anof::http::internal {
// 共用线上表示，避免账户管理与首批身份/空间响应产生字段差异。
std::string userTag(const UserProfile& user);
Result<nlohmann::json> userJson(const UserProfile& user);
nlohmann::json quotaJson(const QuotaSnapshot& quota);
}  // namespace anof::http::internal
