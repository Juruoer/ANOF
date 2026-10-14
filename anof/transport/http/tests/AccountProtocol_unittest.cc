#define BOOST_TEST_MODULE AccountProtocol
#include <boost/test/unit_test.hpp>
#include <anof/transport/http/AccountRequest.h>
#include <anof/transport/http/AccountResponse.h>
#include <anof/transport/http/Pagination.h>
#include <nlohmann/json.hpp>
#include <limits>
#include <type_traits>

using namespace anof;
using namespace anof::http;
using muduox::net::HttpHeaders;
using Json = nlohmann::json;
namespace {
const std::string uid = "abcdef00-0000-4000-8000-000000000001";
const std::string sid = "10000000-0000-4000-8000-000000000001";
const auto user = UserId::parse(uid).value();
const auto space = SpaceId::parse(sid).value();
const AccountCondition expected = UserCondition{user, Revision::from(1).value()};
const QuotaScope scope = SpaceQuota{space};
const auto source = AuthenticationSource::ipv4({127, 0, 0, 1});
const std::string maximum = "9223372036854775807";
template <typename T>
void invalid(const Parsed<T>& value, ProtocolError error = ProtocolError::InvalidArgument) {
    BOOST_REQUIRE(std::holds_alternative<ProtocolError>(value));
    BOOST_CHECK(std::get<ProtocolError>(value) == error);
}
Parsed<AccountCondition> condition(const std::string& tag) {
    HttpHeaders headers; headers.append("If-Match", tag); return parseAccountCondition(headers);
}
std::string quota(const std::string& bytes, const std::string& nodes) {
    return Json{{"limit", {{"bytes", bytes}, {"nodes", nodes}}}}.dump();
}
}
BOOST_AUTO_TEST_CASE(threeStrongConditionsPreserveBindingAndBoundaryRevision) {
    const auto a = condition(" \t\"user-" + uid + "-1\"\t ");
    BOOST_REQUIRE(std::holds_alternative<AccountCondition>(a));
    auto u = std::get<UserCondition>(std::get<AccountCondition>(a));
    BOOST_CHECK(u.id == user); BOOST_CHECK_EQUAL(u.revision.value(), 1);
    const auto b = condition("\"quota-space-" + sid + "-" + maximum + "\"");
    BOOST_REQUIRE(std::holds_alternative<AccountCondition>(b));
    auto q = std::get<QuotaCondition>(std::get<AccountCondition>(b));
    BOOST_CHECK(q.scope == scope); BOOST_CHECK_EQUAL(q.revision.value(), std::numeric_limits<std::int64_t>::max());
    const auto c = condition("\"quota-contribution-" + sid + "-" + uid + "-2\"");
    BOOST_REQUIRE(std::holds_alternative<AccountCondition>(c));
    q = std::get<QuotaCondition>(std::get<AccountCondition>(c));
    BOOST_CHECK(q.scope == QuotaScope(PublicContributionQuota{space, user}));
    BOOST_CHECK_EQUAL(q.revision.value(), 2);
}
BOOST_AUTO_TEST_CASE(missingAndDuplicateConditionsAreDistinct) {
    HttpHeaders headers; invalid(parseAccountCondition(headers), ProtocolError::PreconditionRequired);
    headers.append("If-Match", "\"user-" + uid + "-1\"");
    headers.append("if-match", "\"user-" + uid + "-1\"");
    invalid(parseAccountCondition(headers));
    invalid(condition("")); invalid(condition(" \t"));
}
BOOST_AUTO_TEST_CASE(conditionRejectsWeakListsWildcardAndNoncanonicalNumbers) {
    const std::string tag = "\"user-" + uid + "-1\"";
    for (const auto& value : {std::string("*"), "W/" + tag, "w/" + tag, tag + ", " + tag, tag + ",", "user-" + uid + "-1", "\n" + tag, tag + "\r", tag + "x"}) invalid(condition(value));
    for (const auto& prefix : {"user-" + uid + "-", "quota-space-" + sid + "-", "quota-contribution-" + sid + "-" + uid + "-"}) {
        for (const auto* revision : {"", "0", "01", "-1", "+1", "1.0", "1e1", " 1", "1 ", "9223372036854775808", "18446744073709551616"})
            invalid(condition("\"" + prefix + revision + "\""));
    }
}
BOOST_AUTO_TEST_CASE(conditionRejectsNoncanonicalIdsAndWrongStructure) {
    for (const auto* id : {"ABCDEF00-0000-4000-8000-000000000001", "00000000-0000-0000-0000-000000000000", "bad", "%61bcdef00-0000-4000-8000-000000000001"}) {
        invalid(condition("\"user-" + std::string(id) + "-1\""));
        invalid(condition("\"quota-space-" + std::string(id) + "-1\""));
        invalid(condition("\"quota-contribution-" + sid + "-" + id + "-1\""));
        invalid(condition("\"quota-contribution-" + std::string(id) + "-" + uid + "-1\""));
    }
    for (const auto& raw : {"node-" + uid + "-1", "quota-" + sid + "-1", "quota-contribution-" + sid + "-1", "user-" + uid + "-" + sid + "-1"}) invalid(condition("\"" + raw + "\""));
    invalid(condition(std::string("\"user-") + uid + "-1" + std::string(1, '\0') + "\""));
}
BOOST_AUTO_TEST_CASE(validMismatchedConditionsRemainOwnedAndUnchanged) {
    auto raw = "\"quota-contribution-" + sid + "-" + uid + "-7\"";
    auto parsed = condition(raw); raw.assign(raw.size(), 'x');
    const auto binding = std::get<AccountCondition>(parsed);
    auto rename = parseUpdateDisplayNameJson(R"({"display_name":"家庭"})", binding);
    BOOST_REQUIRE(std::holds_alternative<UpdateDisplayNameCommand>(rename));
    const auto q = std::get<QuotaCondition>(std::get<UpdateDisplayNameCommand>(rename).expected);
    BOOST_CHECK(q.scope == QuotaScope(PublicContributionQuota{space, user}));
    BOOST_CHECK_EQUAL(q.revision.value(), 7);
    auto enabled = parseSetUserEnabledJson(R"({"enabled":false})", user, binding);
    BOOST_REQUIRE(std::holds_alternative<SetUserEnabledCommand>(enabled));
    BOOST_CHECK(std::holds_alternative<QuotaCondition>(std::get<SetUserEnabledCommand>(enabled).expected));
    auto password = parseChangePasswordJson(R"({"current_password":"example-password","new_password":"example-new-password"})", binding, source);
    BOOST_REQUIRE(std::holds_alternative<ChangePasswordCommand>(password));
    BOOST_CHECK(std::holds_alternative<QuotaCondition>(std::get<ChangePasswordCommand>(password).expected));
    auto limit = parseSetQuotaLimitJson(quota("0", "1"), scope, expected);
    BOOST_REQUIRE(std::holds_alternative<SetQuotaLimitCommand>(limit));
    BOOST_CHECK(std::get<UserCondition>(std::get<SetQuotaLimitCommand>(limit).expected).id == user);
}
BOOST_AUTO_TEST_CASE(displayNameValidatesDecodedUtf8BytesAndSchema) {
    for (const auto& name : {std::string("家庭"), std::string(128, 'a')}) {
        auto result = parseUpdateDisplayNameJson(Json{{"display_name", name}}.dump(), expected);
        BOOST_REQUIRE(std::holds_alternative<UpdateDisplayNameCommand>(result));
        BOOST_CHECK_EQUAL(std::get<UpdateDisplayNameCommand>(result).displayName.value(), name);
    }
    for (const auto& name : {std::string{}, std::string(129, 'a'), std::string(127, 'a') + "家", std::string("a\nb")})
        invalid(parseUpdateDisplayNameJson(Json{{"display_name", name}}.dump(), expected));
    for (const auto* body : {"{}", "[]", "null", R"({"display_name":true})", R"({"display_name":"x","login":"bob"})", R"({"display_name":"x","display_\u006eame":"y"})", R"({"display_name":{}})", R"({"display_name":"\ud800"})", R"({"display_name":"x"}{})"}) invalid(parseUpdateDisplayNameJson(body, expected));
    invalid(parseUpdateDisplayNameJson(std::string("{\"display_name\":\"") + char(0xff) + "\"}", expected));
}
BOOST_AUTO_TEST_CASE(passwordCommandsOwnBothSecretsAndTrustedSource) {
    static_assert(!std::is_copy_constructible_v<ChangePasswordCommand>);
    auto body = Json{{"current_password", "  password  "}, {"new_password", std::string(1024, 'n')}}.dump();
    auto result = parseChangePasswordJson(body, expected, source); body.assign(body.size(), 'x');
    BOOST_REQUIRE(std::holds_alternative<ChangePasswordCommand>(result));
    auto cmd = std::get<ChangePasswordCommand>(std::move(result));
    BOOST_CHECK_EQUAL(cmd.currentPassword.view(), "  password  ");
    BOOST_CHECK_EQUAL(cmd.newPassword.view(), std::string(1024, 'n'));
    BOOST_CHECK(cmd.source == source);
    BOOST_CHECK(std::get<UserCondition>(cmd.expected).id == user);
    for (const auto& name : {"current_password", "new_password"}) {
        for (const auto& value : {std::string(11, 'x'), std::string(1025, 'x'), std::string(1023, 'x') + "家", std::string(12, '\0')}) {
            Json json{{"current_password", "  password  "}, {"new_password", "  password  "}}; json[name] = value;
            invalid(parseChangePasswordJson(json.dump(), expected, source));
        }
    }
}
BOOST_AUTO_TEST_CASE(passwordAndEnabledRejectMistypedMissingAndExtraFields) {
    for (const auto* body : {"{}", R"({"current_password":"example-password"})", R"({"current_password":false,"new_password":"example-password"})", R"({"current_password":"example-password","new_password":"example-password","source":"127.0.0.1"})", R"({"current_password":"example-password","current_\u0070assword":"example-password","new_password":"example-password"})"}) invalid(parseChangePasswordJson(body, expected, source));
    for (const bool value : {false, true}) {
        auto result = parseSetUserEnabledJson(Json{{"enabled", value}}.dump(), user, expected);
        BOOST_REQUIRE(std::holds_alternative<SetUserEnabledCommand>(result));
        BOOST_CHECK_EQUAL(std::get<SetUserEnabledCommand>(result).enabled, value);
    }
    for (const auto* body : {"{}", R"({"enabled":0})", R"({"enabled":"true"})", R"({"enabled":null})", R"({"enabled":true,"role":"member"})", R"({"enabled":false,"enabl\u0065d":true})", R"({"enabled":[]})"}) invalid(parseSetUserEnabledJson(body, user, expected));
}
BOOST_AUTO_TEST_CASE(quotaDecimalBoundsAndBothScopes) {
    for (const QuotaScope target : {scope, QuotaScope(PublicContributionQuota{space, user})}) {
        auto result = parseSetQuotaLimitJson(quota("0", maximum), target, expected);
        BOOST_REQUIRE(std::holds_alternative<SetQuotaLimitCommand>(result));
        const auto& cmd = std::get<SetQuotaLimitCommand>(result);
        BOOST_CHECK(cmd.scope == target); BOOST_CHECK_EQUAL(cmd.limit.bytes.value(), 0);
        BOOST_CHECK_EQUAL(cmd.limit.nodes.value(), std::numeric_limits<std::int64_t>::max());
        BOOST_CHECK(std::holds_alternative<SetQuotaLimitCommand>(parseSetQuotaLimitJson(quota(maximum, "0"), target, expected)));
    }
    for (const auto* value : {"", "00", "01", "+1", "-1", "1.0", "1e3", " 1", "1 ", "9223372036854775808", "18446744073709551616", "无限"}) {
        invalid(parseSetQuotaLimitJson(quota(value, "0"), scope, expected));
        invalid(parseSetQuotaLimitJson(quota("0", value), scope, expected));
    }
}
BOOST_AUTO_TEST_CASE(quotaRejectsNestedDuplicateUnknownMissingAndMistypedFields) {
    for (const auto* body : {"", "{}", "[]", R"({"limit":{}})", R"({"limit":{"bytes":"0"}})", R"({"limit":{"nodes":"0"}})", R"({"limit":{"bytes":0,"nodes":"0"}})", R"({"limit":{"bytes":"0","nodes":false}})", R"({"limit":{"bytes":"0","nodes":"0","used":"0"}})", R"({"limit":{"bytes":"0","bytes":"1","nodes":"0"}})", R"({"limit":{"bytes":"0","no\u0064es":"0","nodes":"1"}})", R"({"limit":{"bytes":"0","nodes":"0"},"lim\u0069t":{"bytes":"1","nodes":"1"}})", R"({"limit":{"bytes":"0","nodes":"0"},"revision":"1"})", R"({"limit":[]})", R"({"limit":{"bytes":{},"nodes":"0"}})", R"({"limit":{"bytes":"0","nodes":"0"}} true)"}) invalid(parseSetQuotaLimitJson(body, scope, expected));
    BOOST_CHECK(std::holds_alternative<SetQuotaLimitCommand>(parseSetQuotaLimitJson(R"({"limit":{"nodes":"1","by\u0074es":"0"}})", scope, expected)));
}
BOOST_AUTO_TEST_CASE(bodyCapAndNestingRemainBounded) {
    std::string body = R"({"limit":{"bytes":"0","nodes":"0"}})";
    body.append(kMaxJsonBytes - body.size(), ' ');
    BOOST_CHECK(std::holds_alternative<SetQuotaLimitCommand>(parseSetQuotaLimitJson(body, scope, expected)));
    body += ' '; invalid(parseSetQuotaLimitJson(body, scope, expected), ProtocolError::RequestTooLarge);
    invalid(parseUpdateDisplayNameJson(body, expected), ProtocolError::RequestTooLarge);
    invalid(parseChangePasswordJson(body, expected, source), ProtocolError::RequestTooLarge);
    invalid(parseSetUserEnabledJson(body, user, expected), ProtocolError::RequestTooLarge);
    invalid(parseSetQuotaLimitJson(std::string(33, '[') + "0" + std::string(33, ']'), scope, expected));
}
BOOST_AUTO_TEST_CASE(pathInputsAreOwnedAndDoNotAuthorizeScope) {
    auto result = parseGetUser(uid); BOOST_REQUIRE(std::holds_alternative<GetUserCommand>(result));
    BOOST_CHECK(std::get<GetUserCommand>(result).id == user);
    BOOST_CHECK(std::get<QuotaScope>(parseQuotaScope(sid)) == scope);
    BOOST_CHECK(std::get<QuotaScope>(parseQuotaScope(sid, uid)) == QuotaScope(PublicContributionQuota{space, user}));
    invalid(parseGetUser("bad")); invalid(parseQuotaScope("bad")); invalid(parseQuotaScope(sid, ""));
}
namespace {
const auto requestId = RequestId::parse(uid).value();
UserProfile profile() {
    return {user, LoginName::parse("alice").value(), DisplayName::parse("家庭").value(), UserRole::Member, false,
            AuthEpoch::from(9).value(), Revision::from(7).value(), UtcTime{}, UtcTime{}};
}
std::string header(const JsonResponse& response, const char* name) {
    return std::string(response.headers.headers().getSingle(name).value);
}
std::string encoded(std::string_view raw) {
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (std::size_t i = 0; i < raw.size(); i += 3) {
        unsigned word = static_cast<unsigned char>(raw[i]) << 16;
        if (i + 1 < raw.size()) word |= static_cast<unsigned char>(raw[i + 1]) << 8;
        if (i + 2 < raw.size()) word |= static_cast<unsigned char>(raw[i + 2]);
        out += alphabet[(word >> 18) & 63]; out += alphabet[(word >> 12) & 63];
        if (i + 1 < raw.size()) out += alphabet[(word >> 6) & 63];
        if (i + 2 < raw.size()) out += alphabet[word & 63];
    }
    return out;
}
void common(const JsonResponse& response) {
    BOOST_CHECK_EQUAL(header(response, "Cache-Control"), "no-store");
    BOOST_CHECK_EQUAL(header(response, "X-Request-ID"), requestId.toString());
    BOOST_CHECK_EQUAL(header(response, "Content-Type"), "application/json");
    for (const auto* secret : {"password", "auth_epoch", "session_digest", "access_token"}) BOOST_CHECK(response.body.find(secret) == std::string::npos);
}
}
BOOST_AUTO_TEST_CASE(userCursorRoundTripsWithOwnedCallerBinding) {
    const UserCursor cursor{user, {UserId::parse(sid).value()}};
    auto wire = encodeCursor(cursor); BOOST_REQUIRE(wire);
    BOOST_CHECK_EQUAL(wire.value(), encoded(Json{{"v", 1}, {"kind", "users"}, {"user_id", uid}, {"after_id", sid}}.dump()));
    auto decoded = decodeUserCursor(wire.value()); BOOST_REQUIRE(std::holds_alternative<UserCursor>(decoded));
    BOOST_CHECK(std::get<UserCursor>(decoded).userId == user);
    BOOST_CHECK(std::get<UserCursor>(decoded).position.id == cursor.position.id);
    auto command = parseListUsers("?limit=200&cursor=" + wire.value());
    BOOST_REQUIRE(std::holds_alternative<ListUsersCommand>(command));
    const auto& cmd = std::get<ListUsersCommand>(command);
    BOOST_CHECK_EQUAL(cmd.size.value(), 200); BOOST_REQUIRE(cmd.after);
    BOOST_CHECK(cmd.after->userId == user);
    BOOST_CHECK_EQUAL(std::get<ListUsersCommand>(parseListUsers("")).size.value(), 100);
    BOOST_CHECK_EQUAL(std::get<ListUsersCommand>(parseListUsers("?limit=1")).size.value(), 1);
}
BOOST_AUTO_TEST_CASE(userCursorRejectsNoncanonicalEncodingAndInvalidStructure) {
    const Json good{{"v", 1}, {"kind", "users"}, {"user_id", uid}, {"after_id", sid}};
    for (const auto& raw : {std::string{}, std::string("A"), std::string(2049, 'A'), encoded(good.dump()) + "=", encoded(good.dump()) + "+"}) invalid(decodeUserCursor(raw));
    for (const auto* key : {"v", "kind", "user_id", "after_id"}) {
        auto json = good; json.erase(key); invalid(decodeUserCursor(encoded(json.dump())));
        json = good; json[key] = nullptr; invalid(decodeUserCursor(encoded(json.dump())));
    }
    auto json = good; json["v"] = 2; invalid(decodeUserCursor(encoded(json.dump())));
    json = good; json["kind"] = "invites"; invalid(decodeUserCursor(encoded(json.dump())));
    json = good; json["extra"] = 0; invalid(decodeUserCursor(encoded(json.dump())));
    for (const auto* key : {"user_id", "after_id"}) {
        for (const auto* value : {"ABCDEF00-0000-4000-8000-000000000001", "00000000-0000-0000-0000-000000000000", "bad"}) {
            json = good; json[key] = value; invalid(decodeUserCursor(encoded(json.dump())));
        }
    }
    const auto raw = good.dump();
    invalid(decodeUserCursor(encoded(raw.substr(0, raw.size() - 1) + ",\"user_\\u0069d\":\"" + uid + "\"}")));
    invalid(decodeUserCursor(encoded(raw + "{}")));
    invalid(decodeUserCursor(encoded(std::string(1537, ' '))));
    invalid(decodeInviteCursor(encodeCursor(UserCursor{user, {user}}).value()));
    invalid(decodeChildrenCursor(encodeCursor(UserCursor{user, {user}}).value()));
}
BOOST_AUTO_TEST_CASE(usersQueryRejectsUnknownDuplicateAndInvalidLimits) {
    for (const auto* query : {"?limit=0", "?limit=201", "?limit=01", "?limit=-1", "?limit=+1", "?limit=1.0", "?limit=1e2", "?limit=9223372036854775808", "?limit=", "?cursor=", "?limit=1&limit=2", "?cursor=A&cursor=A", "?offset=0", "?limit=1&%6cimit=2", "?cursor=%zz"}) invalid(parseListUsers(query));
}
BOOST_AUTO_TEST_CASE(adminUserResponseReusesProfileAndReturnedUserTag) {
    const auto value = profile();
    auto output = adminUserResponse({value, space}, requestId); BOOST_REQUIRE(output); common(output.value());
    BOOST_CHECK_EQUAL(static_cast<int>(output.value().headers.statusCode()), 200);
    const auto json = Json::parse(output.value().body);
    BOOST_CHECK_EQUAL(json.size(), 2); BOOST_CHECK_EQUAL(json["user"].size(), 8);
    BOOST_CHECK_EQUAL(json["private_space_id"].get<std::string>(), sid);
    BOOST_CHECK_EQUAL(json["user"]["revision"].get<std::string>(), "7");
    BOOST_CHECK_EQUAL(header(output.value(), "ETag"), "\"user-" + uid + "-7\"");
    auto self = currentUserResponse(value, requestId); BOOST_REQUIRE(self);
    BOOST_CHECK(Json::parse(self.value().body) == json["user"]);
    BOOST_CHECK_EQUAL(header(self.value(), "ETag"), header(output.value(), "ETag"));
    auto bad = value; bad.role = static_cast<UserRole>(99);
    BOOST_CHECK(!adminUserResponse({bad, space}, requestId));
    const auto empty = noContentResponse(requestId);
    BOOST_CHECK_EQUAL(static_cast<int>(empty.headers.statusCode()), 204); BOOST_CHECK(empty.body.empty());
    BOOST_CHECK(header(empty, "ETag").empty()); BOOST_CHECK(header(empty, "Content-Type").empty());
    BOOST_CHECK_EQUAL(header(empty, "Cache-Control"), "no-store");
    BOOST_CHECK_EQUAL(header(empty, "X-Request-ID"), requestId.toString());
}
BOOST_AUTO_TEST_CASE(managedQuotaResponseMatchesScopeAndCanonicalTag) {
    const QuotaDelta zero{ByteCount::from(0).value(), NodeCount::from(0).value()};
    const QuotaDelta limit{ByteCount::from(std::numeric_limits<std::int64_t>::max()).value(), NodeCount::from(100).value()};
    const QuotaSnapshot snapshot{{limit, zero, zero}, Revision::from(3).value()};
    for (const QuotaScope target : {scope, QuotaScope(PublicContributionQuota{space, user})}) {
        auto output = managedQuotaResponse({target, snapshot}, requestId); BOOST_REQUIRE(output); common(output.value());
        const auto json = Json::parse(output.value().body);
        BOOST_CHECK_EQUAL(json.size(), 3); BOOST_CHECK_EQUAL(json["space_id"].get<std::string>(), sid);
        BOOST_CHECK_EQUAL(json["quota"].size(), 4);
        BOOST_CHECK_EQUAL(json["quota"]["limit"]["bytes"].get<std::string>(), maximum);
        BOOST_CHECK_EQUAL(json["quota"]["used"]["bytes"].get<std::string>(), "0");
        BOOST_CHECK_EQUAL(json["quota"]["revision"].get<std::string>(), "3");
        if (std::holds_alternative<SpaceQuota>(target)) BOOST_CHECK(json["user_id"].is_null());
        else BOOST_CHECK_EQUAL(json["user_id"].get<std::string>(), uid);
        const auto parsed = condition(header(output.value(), "ETag"));
        BOOST_REQUIRE(std::holds_alternative<AccountCondition>(parsed));
        const auto binding = std::get<QuotaCondition>(std::get<AccountCondition>(parsed));
        BOOST_CHECK(binding.scope == target); BOOST_CHECK_EQUAL(binding.revision.value(), 3);
    }
}
BOOST_AUTO_TEST_CASE(usersPageHasNoCollectionTagAndValidatesLastPosition) {
    Page<UserProfile, UserCursor> page{{profile()}, UserCursor{UserId::parse(sid).value(), {user}}};
    auto output = usersResponse(page, requestId); BOOST_REQUIRE(output); common(output.value());
    BOOST_CHECK(header(output.value(), "ETag").empty());
    auto json = Json::parse(output.value().body);
    BOOST_CHECK_EQUAL(json["items"].size(), 1);
    auto next = std::get<UserCursor>(decodeUserCursor(json["next_cursor"].get<std::string>()));
    BOOST_CHECK(next.userId == page.next->userId); BOOST_CHECK(next.position.id == user);
    page.next->position.id = UserId::parse(sid).value(); BOOST_CHECK(!usersResponse(page, requestId));
    page.items.clear(); BOOST_CHECK(!usersResponse(page, requestId));
    page.next.reset(); output = usersResponse(page, requestId); BOOST_REQUIRE(output);
    json = Json::parse(output.value().body); BOOST_CHECK(json["items"].empty()); BOOST_CHECK(json["next_cursor"].is_null());
    // 验证数量上限；排序和唯一性由应用负责。
    page.items.assign(200, profile()); BOOST_CHECK(usersResponse(page, requestId));
    page.items.push_back(profile()); BOOST_CHECK(!usersResponse(page, requestId));
}
BOOST_AUTO_TEST_CASE(quotaOperationErrorsPreserveOtherStatusesAndSecretFreeEnvelope) {
    auto mkdir = errorResponse(protocolError(ErrorCode::QuotaExceeded), requestId);
    auto quotaError = errorResponse(quotaUpdateError(ErrorCode::QuotaExceeded), requestId);
    BOOST_CHECK_EQUAL(static_cast<int>(mkdir.headers.statusCode()), 403);
    BOOST_CHECK_EQUAL(static_cast<int>(quotaError.headers.statusCode()), 409);
    BOOST_CHECK_EQUAL(Json::parse(quotaError.body)["error"]["code"].get<std::string>(), "QUOTA_EXCEEDED");
    for (const auto& entry : {std::pair{ErrorCode::Unauthenticated, 401}, {ErrorCode::Forbidden, 403}, {ErrorCode::NotFound, 404}, {ErrorCode::PreconditionFailed, 412}, {ErrorCode::PreconditionRequired, 428}, {ErrorCode::RateLimited, 429}, {ErrorCode::Retryable, 503}, {ErrorCode::IntegrityError, 500}, {ErrorCode::CommitOutcomeUnknown, 500}}) {
        const auto output = errorResponse(quotaUpdateError(entry.first), requestId); common(output);
        BOOST_CHECK_EQUAL(static_cast<int>(output.headers.statusCode()), entry.second);
        BOOST_CHECK(header(output, "ETag").empty());
        const auto json = Json::parse(output.body); BOOST_CHECK_EQUAL(json.size(), 1);
        BOOST_CHECK_EQUAL(json["error"].size(), 4); BOOST_CHECK(json["error"]["details"].empty());
        if (entry.first == ErrorCode::IntegrityError) BOOST_CHECK_EQUAL(json["error"]["code"].get<std::string>(), "INTERNAL_ERROR");
        if (entry.first == ErrorCode::CommitOutcomeUnknown) BOOST_CHECK_EQUAL(json["error"]["code"].get<std::string>(), "COMMIT_OUTCOME_UNKNOWN");
    }
}
