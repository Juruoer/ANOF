#define BOOST_TEST_MODULE IdentityProtocol
#include <boost/test/unit_test.hpp>
#include <anof/transport/http/IdentityRequest.h>
#include <anof/transport/http/IdentityResponse.h>
#include <anof/transport/http/Pagination.h>
#include <muduox/net/Buffer.h>
#include <nlohmann/json.hpp>

#include <limits>
#include <type_traits>

using namespace anof;
using namespace anof::http;
using namespace muduox::net;
using Json = nlohmann::json;
namespace {
const std::string userId = "40000000-0000-4000-8000-000000000001";
const std::string spaceId = "10000000-0000-4000-8000-000000000001";
const std::string parentId = "20000000-0000-4000-8000-000000000001";
const std::string nodeId = "30000000-0000-4000-8000-000000000001";
const std::string inviteId = "50000000-0000-4000-8000-000000000001";
const std::string token(43, 'A');
const std::string inviteToken = "anof_invite_" + token;
const std::string timestamp = "2026-10-08T08:00:00.000000Z";
RequestId requestId() { return RequestId::parse(inviteId).value(); }
AuthenticationSource source() { return AuthenticationSource::ipv4({127, 0, 0, 1}); }
UtcTime timeValue() { return parseUtcTimestamp(timestamp).value(); }
UserProfile user() {
    return {UserId::parse(userId).value(), LoginName::parse("alice").value(), DisplayName::parse("家庭").value(), UserRole::Member, true,
            AuthEpoch::from(7).value(), Revision::from(9).value(), timeValue(), timeValue()};
}
InviteInfo invite() {
    return {InviteId::parse(inviteId).value(), UserId::parse(userId).value(), timeValue(), timeValue() + std::chrono::hours(24), InviteUseLimit::from(10).value(), InviteUseCount::from(2).value(), std::nullopt};
}
NodeDetails node() {
    return {{NodeId::parse(nodeId).value(), SpaceId::parse(spaceId).value(), NodeId::parse(parentId).value(), "家庭", UserId::parse(userId).value(), Revision::from(9).value(), timeValue(), timeValue()}, DirectoryContent{}};
}
ChildrenCursor childrenCursor() {
    return {UserId::parse(userId).value(), SpaceId::parse(spaceId).value(), NodeId::parse(parentId).value(), {"家庭", NodeId::parse(nodeId).value()}};
}
InviteCursor inviteCursor() { return {UserId::parse(userId).value(), {timeValue(), InviteId::parse(inviteId).value()}}; }
Json registerBody() { return {{"login", "Alice"}, {"display_name", "家庭"}, {"password", "example-only-password"}, {"invite_token", inviteToken}}; }
Json loginBody() { return {{"login", "Alice"}, {"password", "example-only-password"}, {"credential_mode", "bearer"}}; }
Json childCursorJson() {
    return {{"v", 1}, {"kind", "children"}, {"user_id", userId}, {"space_id", spaceId}, {"parent_id", parentId}, {"after_name", "家庭"}, {"after_id", nodeId}};
}
Json inviteCursorJson() { return {{"v", 1}, {"kind", "invites"}, {"user_id", userId}, {"after_created_at", timestamp}, {"after_id", inviteId}}; }
// 测试用三字节分组编码，用于构造不合法的内部 JSON；另有固定 wire 向量验证生产编码。
std::string encoded(std::string_view bytes) {
    constexpr char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        unsigned word = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) word |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size()) word |= static_cast<unsigned char>(bytes[i + 2]);
        out += chars[(word >> 18) & 63];
        out += chars[(word >> 12) & 63];
        if (i + 1 < bytes.size()) out += chars[(word >> 6) & 63];
        if (i + 2 < bytes.size()) out += chars[word & 63];
    }
    return out;
}
template <typename T>
void invalid(const Parsed<T>& result, ProtocolError expected = ProtocolError::InvalidArgument) {
    BOOST_REQUIRE(std::holds_alternative<ProtocolError>(result));
    BOOST_CHECK(std::get<ProtocolError>(result) == expected);
}
void headers(const JsonResponse& response, int status) {
    BOOST_CHECK_EQUAL(static_cast<int>(response.headers.statusCode()), status);
    BOOST_CHECK_EQUAL(response.headers.headers().getSingle("X-Request-ID").value, requestId().toString());
    BOOST_CHECK_EQUAL(response.headers.headers().getSingle("Cache-Control").value, "no-store");
    BOOST_CHECK(!response.headers.headers().contains("Set-Cookie"));
    BOOST_CHECK(!response.headers.headers().contains("Location"));
    if (status != 204) BOOST_CHECK_EQUAL(response.headers.headers().getSingle("Content-Type").value, "application/json");
}
Json body(const Result<JsonResponse>& output) {
    BOOST_REQUIRE(output);
    return Json::parse(output.value().body);
}
}  // namespace

BOOST_AUTO_TEST_CASE(commandsOwnSecretsAndNormalizeLoginOnly) {
    static_assert(!std::is_copy_constructible_v<RegisterCommand>);
    static_assert(!std::is_copy_constructible_v<LoginCommand>);
    auto input = registerBody();
    input["display_name"] = " 家庭 ";
    input["password"] = "  password  ";
    auto text = input.dump();
    auto parsed = parseRegisterJson(text, source());
    text.assign(text.size(), 'x');
    BOOST_REQUIRE(std::holds_alternative<RegisterCommand>(parsed));
    const auto& command = std::get<RegisterCommand>(parsed);
    BOOST_CHECK_EQUAL(command.login.value(), "alice");
    BOOST_CHECK_EQUAL(command.displayName.value(), " 家庭 ");
    BOOST_CHECK_EQUAL(command.password.view(), "  password  ");
    BOOST_CHECK_EQUAL(command.invite.view(), inviteToken);
    BOOST_CHECK(command.source == source());
    auto login = parseLoginJson(loginBody().dump(), source());
    BOOST_REQUIRE(std::holds_alternative<LoginCommand>(login));
    BOOST_CHECK(std::get<LoginCommand>(login).mode == CredentialMode::Bearer);
    BOOST_CHECK(std::get<LoginCommand>(login).source == source());
}
BOOST_AUTO_TEST_CASE(identityJsonStructureAndFieldTypes) {
    for (const auto& input : {"", "null", "[]", "42", "{}", "{}{}", "/*x*/{}", "{", "{\"login\":null}"}) {
        invalid(parseRegisterJson(input, source()));
        invalid(parseLoginJson(input, source()));
    }
    for (const auto& name : {"login", "password", "credential_mode"}) {
        for (const Json& value : {Json(nullptr), Json(true), Json(1), Json(1.5), Json::array(), Json::object()}) {
            auto input = loginBody(); input[name] = value;
            invalid(parseLoginJson(input.dump(), source()));
        }
        auto input = loginBody(); input.erase(name);
        invalid(parseLoginJson(input.dump(), source()));
    }
    for (const auto& name : {"login", "display_name", "password", "invite_token"}) {
        auto input = registerBody(); input.erase(name);
        invalid(parseRegisterJson(input.dump(), source()));
        input = registerBody(); input[name] = false;
        invalid(parseRegisterJson(input.dump(), source()));
    }
    auto text = loginBody().dump(); text.pop_back();
    for (const auto& extra : {",\"login\":\"alice\"}", ",\"log\\u0069n\":\"alice\"}", ",\"request_id\":\"x\"}", ",\"source\":\"127.0.0.1\"}"}) invalid(parseLoginJson(text + extra, source()));
    text = registerBody().dump(); text.pop_back();
    invalid(parseRegisterJson(text + ",\"invite_token\":\"" + inviteToken + "\"}", source()));
    for (const auto& mode : {"Bearer", "cookie", "", " bearer"}) {
        auto input = loginBody(); input["credential_mode"] = mode;
        invalid(parseLoginJson(input.dump(), source()));
    }
}
BOOST_AUTO_TEST_CASE(decodedByteLimitsAndInvalidUnicode) {
    for (std::size_t size : {11U, 12U, 1024U, 1025U}) {
        auto input = loginBody(); input["password"] = std::string(size, 'p');
        const auto result = parseLoginJson(input.dump(), source());
        BOOST_CHECK_EQUAL(std::holds_alternative<LoginCommand>(result), size >= 12 && size <= 1024);
    }
    auto input = registerBody();
    std::string display;
    for (int i = 0; i < 42; ++i) display += "中";
    input["display_name"] = display + "ab";
    BOOST_CHECK(std::holds_alternative<RegisterCommand>(parseRegisterJson(input.dump(), source())));
    input["display_name"] = display + "中";
    invalid(parseRegisterJson(input.dump(), source()));
    for (const auto& password : {std::string("12345678901\0", 12), std::string(11, 'p')}) {
        input = registerBody(); input["password"] = password;
        invalid(parseRegisterJson(input.dump(), source()));
    }
    input = registerBody(); input["password"] = "中文中文";
    BOOST_CHECK(std::holds_alternative<RegisterCommand>(parseRegisterJson(input.dump(), source())));
    input["password"] = "中文中";
    invalid(parseRegisterJson(input.dump(), source()));
    for (const auto& login : {"ab", "1alice", " alice", "álîce", "alice!"}) {
        input = registerBody(); input["login"] = login;
        invalid(parseRegisterJson(input.dump(), source()));
    }
    for (const auto& inviteText : {token, inviteToken + "=", "anof_invite_" + std::string(42, 'A') + "B"}) {
        input = registerBody(); input["invite_token"] = inviteText;
        invalid(parseRegisterJson(input.dump(), source()));
    }
    for (const auto& bad : {std::string("\\ud800"), std::string("\xc0\xaf", 2)}) {
        invalid(parseLoginJson("{\"login\":\"alice\",\"password\":\"" + bad + "\",\"credential_mode\":\"bearer\"}", source()));
    }
    auto escaped = loginBody().dump();
    const auto position = escaped.find("example-only-password");
    escaped.replace(position, 21, "\\u4e2d\\u6587\\u4e2d\\u6587");
    BOOST_CHECK(std::holds_alternative<LoginCommand>(parseLoginJson(escaped, source())));
}
BOOST_AUTO_TEST_CASE(jsonSizeAndNestingAreBounded) {
    for (const auto& original : {registerBody().dump(), loginBody().dump(), std::string("{}")}) {
        auto input = original; input.resize(kMaxJsonBytes, ' ');
        if (original == "{}") BOOST_CHECK(std::holds_alternative<CreateInviteCommand>(parseCreateInviteJson(input)));
        else if (original.find("display_name") != std::string::npos) BOOST_CHECK(std::holds_alternative<RegisterCommand>(parseRegisterJson(input, source())));
        else BOOST_CHECK(std::holds_alternative<LoginCommand>(parseLoginJson(input, source())));
        input += ' ';
        invalid(parseRegisterJson(input, source()), ProtocolError::RequestTooLarge);
        invalid(parseLoginJson(input, source()), ProtocolError::RequestTooLarge);
        invalid(parseCreateInviteJson(input), ProtocolError::RequestTooLarge);
    }
    for (std::size_t depth : {2U, 32U, 33U, 10000U}) {
        const auto input = "{\"password\":" + std::string(depth, '[') + "0" + std::string(depth, ']') + "}";
        invalid(parseRegisterJson(input, source()));
        invalid(parseLoginJson(input, source()));
    }
}
BOOST_AUTO_TEST_CASE(inviteDefaultsAndIntegerBounds) {
    auto parsed = parseCreateInviteJson("{}");
    BOOST_REQUIRE(std::holds_alternative<CreateInviteCommand>(parsed));
    BOOST_CHECK_EQUAL(std::get<CreateInviteCommand>(parsed).maxUses.value(), 1);
    BOOST_CHECK_EQUAL(std::get<CreateInviteCommand>(parsed).validFor.value(), 604800);
    for (auto uses : {1, 10000}) for (auto lifetime : {1, 2592000}) {
        parsed = parseCreateInviteJson(Json{{"max_uses", uses}, {"valid_for_seconds", lifetime}}.dump());
        BOOST_REQUIRE(std::holds_alternative<CreateInviteCommand>(parsed));
        BOOST_CHECK_EQUAL(std::get<CreateInviteCommand>(parsed).maxUses.value(), uses);
        BOOST_CHECK_EQUAL(std::get<CreateInviteCommand>(parsed).validFor.value(), lifetime);
    }
    for (const auto& text : {"", "null", "[]", "{\"max_uses\":0}", "{\"max_uses\":10001}", "{\"valid_for_seconds\":2592001}", "{\"max_uses\":-1}", "{\"max_uses\":1.0}", "{\"max_uses\":1e0}", "{\"max_uses\":true}", "{\"max_uses\":\"1\"}", "{\"max_uses\":18446744073709551615}", "{\"max_uses\":18446744073709551616}", "{\"max_uses\":1,\"max_uses\":2}", "{\"expires_at\":\"x\"}"}) invalid(parseCreateInviteJson(text));
}
BOOST_AUTO_TEST_CASE(anonymousCredentialsBodylessFramingAndPathIds) {
    HttpHeaders head;
    BOOST_CHECK(!validateAnonymousCredentials(head));
    BOOST_CHECK(!validateBodylessHeaders(head));
    for (const auto& name : {"AUTHORIZATION", "Cookie"}) {
        head.clear(); head.append(name, "");
        BOOST_CHECK(validateAnonymousCredentials(head).has_value());
    }
    for (const auto& length : {"0", "00", "\t0 "}) {
        head.clear(); head.append("Content-Length", length);
        BOOST_CHECK(!validateBodylessHeaders(head));
    }
    for (const auto& length : {"", "1", "-0", "+0", "0,0", "1e0"}) {
        head.clear(); head.append("Content-Length", length);
        BOOST_CHECK(validateBodylessHeaders(head).has_value());
    }
    head.clear(); head.append("Content-Length", "0"); head.append("content-length", "0");
    BOOST_CHECK(validateBodylessHeaders(head).has_value());
    for (const auto& name : {"Transfer-Encoding", "Trailer"}) {
        head.clear(); head.append(name, "");
        BOOST_CHECK(validateBodylessHeaders(head).has_value());
    }
    head.clear();
    BOOST_CHECK(!validateBodylessContent("", head));
    BOOST_CHECK(validateBodylessContent("x", head).has_value());
    head.append("X-Trailer", "x");
    BOOST_CHECK(validateBodylessContent("", head).has_value());
    BOOST_CHECK(!validateNoQuery(""));
    BOOST_CHECK(!validateNoQuery("?"));
    for (const auto& query : {"?x=1", "limit=1", "=", "%zz", "#fragment"}) BOOST_CHECK(validateNoQuery(query).has_value());
    BOOST_CHECK(std::holds_alternative<GetNodeCommand>(parseGetNode(nodeId)));
    BOOST_CHECK(std::holds_alternative<RevokeInviteCommand>(parseRevokeInvite(inviteId)));
    const std::string upper = "AAAAAAAA-0000-4000-8000-000000000001";
    BOOST_CHECK_EQUAL(std::get<GetNodeCommand>(parseGetNode(upper)).id.toString(), "aaaaaaaa-0000-4000-8000-000000000001");
    for (const auto& bad : {"", "bad", "00000000-0000-0000-0000-000000000000", "%34"}) {
        invalid(parseGetNode(bad)); invalid(parseRevokeInvite(bad));
    }
}
BOOST_AUTO_TEST_CASE(authenticationSourceUsesDeclaredPeerFamilyOnly) {
    HttpRequestMetadata metadata;
    invalid(readAuthenticationSource(metadata), ProtocolError::InternalError);
    metadata.connection.local = HttpEndpoint{HttpEndpoint::Family::IPv4, "127.0.0.1", 80, 0};
    invalid(readAuthenticationSource(metadata), ProtocolError::InternalError);
    metadata.connection.peer = HttpEndpoint{HttpEndpoint::Family::IPv4, "127.0.0.1", 100, 0};
    auto first = readAuthenticationSource(metadata);
    BOOST_REQUIRE(std::holds_alternative<AuthenticationSource>(first));
    BOOST_CHECK(std::get<AuthenticationSource>(first) == source());
    metadata.connection.peer->port = 200;
    BOOST_CHECK(std::get<AuthenticationSource>(readAuthenticationSource(metadata)) == source());
    metadata.connection.peer = HttpEndpoint{HttpEndpoint::Family::IPv6, "::ffff:127.0.0.1", 300, 0};
    BOOST_CHECK(std::get<AuthenticationSource>(readAuthenticationSource(metadata)) == source());
    metadata.connection.peer->ip = "2001:db8::1";
    BOOST_CHECK(std::holds_alternative<AuthenticationSource>(readAuthenticationSource(metadata)));
    for (const auto& bad : {std::string("127.0.0.1"), std::string("[::1]"), std::string("::1%lo"), std::string("::1\0bad", 7), std::string{}}) {
        metadata.connection.peer->ip = bad;
        invalid(readAuthenticationSource(metadata), ProtocolError::InternalError);
    }
    metadata.connection.peer = HttpEndpoint{HttpEndpoint::Family::IPv4, "::1", 0, 0};
    invalid(readAuthenticationSource(metadata), ProtocolError::InternalError);
    metadata.connection.peer->ip = "127.000.0.1";
    invalid(readAuthenticationSource(metadata), ProtocolError::InternalError);
}

BOOST_AUTO_TEST_CASE(cursorRoundTripsAndFixedWireVector) {
    // 独立固定向量，字段顺序不影响解码。
    const std::string wire = "eyJ2IjoxLCJraW5kIjoiaW52aXRlcyIsInVzZXJfaWQiOiI0MDAwMDAwMC0wMDAwLTQwMDAtODAwMC0wMDAwMDAwMDAwMDEiLCJhZnRlcl9jcmVhdGVkX2F0IjoiMjAyNi0xMC0wOFQwODowMDowMC4wMDAwMDBaIiwiYWZ0ZXJfaWQiOiI1MDAwMDAwMC0wMDAwLTQwMDAtODAwMC0wMDAwMDAwMDAwMDEifQ";
    auto decoded = decodeInviteCursor(wire);
    BOOST_REQUIRE(std::holds_alternative<InviteCursor>(decoded));
    BOOST_CHECK(std::get<InviteCursor>(decoded).position.createdAt == timeValue());
    BOOST_CHECK_EQUAL(std::get<InviteCursor>(decoded).position.id.toString(), inviteId);
    const auto child = encodeCursor(childrenCursor());
    BOOST_REQUIRE(child);
    BOOST_CHECK_EQUAL(child.value(), encoded(childCursorJson().dump()));
    auto childDecoded = decodeChildrenCursor(child.value());
    BOOST_REQUIRE(std::holds_alternative<ChildrenCursor>(childDecoded));
    const auto& value = std::get<ChildrenCursor>(childDecoded);
    BOOST_CHECK_EQUAL(value.userId.toString(), userId);
    BOOST_CHECK_EQUAL(value.spaceId.toString(), spaceId);
    BOOST_CHECK_EQUAL(value.parent.toString(), parentId);
    BOOST_CHECK_EQUAL(value.position.id.toString(), nodeId);
    BOOST_CHECK_EQUAL(value.position.name, "家庭");
    const auto next = encodeCursor(inviteCursor());
    BOOST_REQUIRE(next);
    BOOST_CHECK_EQUAL(next.value(), encoded(inviteCursorJson().dump()));
    BOOST_CHECK(std::holds_alternative<InviteCursor>(decodeInviteCursor(next.value())));
    invalid(decodeChildrenCursor(next.value()));
    invalid(decodeInviteCursor(child.value()));
}
BOOST_AUTO_TEST_CASE(cursorBase64BoundsAndCanonicalTailBits) {
    auto decoded = childCursorJson().dump();
    decoded.resize(kMaxDecodedCursorBytes, ' ');
    auto wire = encoded(decoded);
    BOOST_CHECK_EQUAL(wire.size(), kMaxCursorBytes);
    BOOST_CHECK(std::holds_alternative<ChildrenCursor>(decodeChildrenCursor(wire)));
    decoded += ' ';
    invalid(decodeChildrenCursor(encoded(decoded)));
    for (const auto& bad : {std::string{}, std::string("A"), std::string("AA="), std::string("AA+"), std::string("AA/"), std::string("AA\n"), std::string(2049, 'A')}) invalid(decodeChildrenCursor(bad));
    // 固定最后一个字符的低位必须为零；解码字节相同也不能接受别名编码。
    decoded = inviteCursorJson().dump() + " ";
    while (decoded.size() % 3 != 1) decoded += ' ';
    wire = encoded(decoded);
    BOOST_REQUIRE_EQUAL(wire.back(), 'A'); // 最后一字节是空格，尾部四位为零。
    wire.back() = 'B';
    invalid(decodeInviteCursor(wire));
}
BOOST_AUTO_TEST_CASE(cursorStructureAndCanonicalValues) {
    auto json = childCursorJson();
    for (const auto& name : {"v", "kind", "user_id", "space_id", "parent_id", "after_name", "after_id"}) {
        auto changed = json; changed.erase(name);
        invalid(decodeChildrenCursor(encoded(changed.dump())));
        changed = json; changed[name] = nullptr;
        invalid(decodeChildrenCursor(encoded(changed.dump())));
    }
    for (const Json& version : {Json(0), Json(2), Json("1"), Json(1.0), Json(true)}) {
        auto changed = json; changed["v"] = version;
        invalid(decodeChildrenCursor(encoded(changed.dump())));
    }
    for (const auto& name : {"user_id", "space_id", "parent_id", "after_id"}) {
        auto changed = json; changed[name] = "AAAAAAAA-0000-4000-8000-000000000001";
        invalid(decodeChildrenCursor(encoded(changed.dump())));
        changed[name] = "00000000-0000-0000-0000-000000000000";
        invalid(decodeChildrenCursor(encoded(changed.dump())));
    }
    for (const auto& name : {"", ".", "..", "a/b"}) {
        auto changed = json; changed["after_name"] = name;
        invalid(decodeChildrenCursor(encoded(changed.dump())));
    }
    auto changed = json; changed["after_name"] = std::string(256, 'x');
    invalid(decodeChildrenCursor(encoded(changed.dump())));
    changed = json; changed["extra"] = 1;
    invalid(decodeChildrenCursor(encoded(changed.dump())));
    auto text = json.dump(); text.pop_back();
    invalid(decodeChildrenCursor(encoded(text + ",\"v\":1}")));
    invalid(decodeChildrenCursor(encoded(text + ",\"k\\u0069nd\":\"children\"}")));
    invalid(decodeChildrenCursor(encoded(json.dump() + "{}")));
    invalid(decodeChildrenCursor(encoded("{\"after_name\":\"\xc0\xaf\"}")));
    invalid(decodeInviteCursor(encoded("{\"v\":1,\"v\":1}")));
    auto inviteJson = inviteCursorJson(); inviteJson["after_created_at"] = "2026-02-30T00:00:00.000000Z";
    invalid(decodeInviteCursor(encoded(inviteJson.dump())));
    auto invalidCursor = childrenCursor(); invalidCursor.position.name.clear();
    BOOST_CHECK(!encodeCursor(invalidCursor));
    auto invalidInvite = inviteCursor(); invalidInvite.position.createdAt = UtcTime(std::chrono::microseconds(std::numeric_limits<std::int64_t>::max()));
    BOOST_CHECK(!encodeCursor(invalidInvite));
}
BOOST_AUTO_TEST_CASE(paginationQueryLimitsAndBindingAreSeparated) {
    auto result = parseListChildren(parentId, "");
    BOOST_REQUIRE(std::holds_alternative<ListChildrenCommand>(result));
    BOOST_CHECK_EQUAL(std::get<ListChildrenCommand>(result).size.value(), 100);
    BOOST_CHECK(!std::get<ListChildrenCommand>(result).after);
    for (const auto& limit : {"1", "100", "200"}) {
        BOOST_CHECK(std::holds_alternative<ListChildrenCommand>(parseListChildren(parentId, "limit=" + std::string(limit))));
        BOOST_CHECK(std::holds_alternative<ListInvitesCommand>(parseListInvites("limit=" + std::string(limit))));
    }
    for (const auto& query : {"limit", "limit=", "limit=0", "limit=201", "limit=01", "limit=+1", "limit=%201", "limit=1.0", "limit=1e2", "limit=-1", "limit=999999999999999999999999", "limit=1&limit=2", "limit=1&%6cimit=2", "cursor=", "cursor=x&cursor=y", "filter=x", "Limit=1", "limit=%XX", "limit=1#x"}) {
        invalid(parseListInvites(query)); invalid(parseListChildren(parentId, query));
    }
    auto cursor = childrenCursor(); cursor.parent = NodeId::parse(nodeId).value();
    auto query = "?limit=2&cursor=" + encodeCursor(cursor).value();
    result = parseListChildren(parentId, query);
    BOOST_REQUIRE(std::holds_alternative<ListChildrenCommand>(result));
    const auto& command = std::get<ListChildrenCommand>(result);
    BOOST_REQUIRE(command.after);
    BOOST_CHECK(command.parent != command.after->parent); // 绑定留给授权后的 application。
    BOOST_CHECK_EQUAL(command.size.value(), 2);
    invalid(parseListChildren("bad", query));
    const auto invites = parseListInvites("cursor=" + encodeCursor(inviteCursor()).value());
    BOOST_REQUIRE(std::holds_alternative<ListInvitesCommand>(invites));
    BOOST_CHECK(std::get<ListInvitesCommand>(invites).after.has_value());
}
BOOST_AUTO_TEST_CASE(utcParsingIsExactAndCalendarChecked) {
    for (const auto& valid : {"0001-01-01T00:00:00.000000Z", "1969-12-31T23:59:59.999999Z", "1970-01-01T00:00:00.000000Z", "2000-02-29T00:00:00.123456Z", "9999-12-31T23:59:59.999999Z"}) {
        auto parsed = parseUtcTimestamp(valid);
        BOOST_REQUIRE(parsed);
        BOOST_CHECK_EQUAL(formatUtcTimestamp(parsed.value()).value(), valid);
    }
    for (const auto& bad : {"0000-01-01T00:00:00.000000Z", "1900-02-29T00:00:00.000000Z", "2026-02-30T00:00:00.000000Z", "2026-13-01T00:00:00.000000Z", "2026-01-00T00:00:00.000000Z", "2026-01-01T24:00:00.000000Z", "2026-01-01T00:60:00.000000Z", "2026-01-01T00:00:60.000000Z", "2026-01-01T00:00:00Z", "2026-01-01T00:00:00.00000Z", "2026-01-01T00:00:00.0000000Z", "2026-01-01T00:00:00.000000+00:00", "2026-01-01t00:00:00.000000Z", "2026-01-01T00:00:00.00000xZ"}) BOOST_CHECK(!parseUtcTimestamp(bad));
}
BOOST_AUTO_TEST_CASE(userAndLoginRepresentationsDoNotExposeInternalFields) {
    const auto registered = registeredUserResponse(user(), requestId());
    const auto json = body(registered);
    headers(registered.value(), 201);
    BOOST_CHECK(!registered.value().headers.headers().contains("ETag"));
    BOOST_CHECK_EQUAL(json.size(), 8);
    BOOST_CHECK_EQUAL(json["id"].get<std::string>(), userId);
    BOOST_CHECK_EQUAL(json["login"].get<std::string>(), "alice");
    BOOST_CHECK_EQUAL(json["display_name"].get<std::string>(), "家庭");
    BOOST_CHECK_EQUAL(json["role"].get<std::string>(), "member");
    BOOST_CHECK_EQUAL(json["revision"].get<std::string>(), "9");
    BOOST_CHECK_EQUAL(json["created_at"].get<std::string>(), timestamp);
    for (const auto& secret : {"auth_epoch", "password_version", "password_hash", "digest", "access_token", "session"}) BOOST_CHECK(!json.contains(secret));
    auto admin = user(); admin.role = UserRole::Administrator;
    const auto current = currentUserResponse(admin, requestId());
    BOOST_CHECK_EQUAL(body(current)["role"].get<std::string>(), "administrator");
    headers(current.value(), 200);
    BOOST_CHECK_EQUAL(current.value().headers.headers().getSingle("ETag").value, "\"user-" + userId + "-9\"");
    LoginResult login{SecretToken::session(token).value(), timeValue(), user()};
    const auto loggedIn = loginResponse(std::move(login), requestId());
    const auto loginJson = body(loggedIn);
    headers(loggedIn.value(), 201);
    BOOST_CHECK_EQUAL(loginJson.size(), 4);
    BOOST_CHECK(loginJson["user"] == json);
    BOOST_CHECK_EQUAL(loginJson["access_token"].get<std::string>(), token);
    BOOST_CHECK_EQUAL(loginJson["token_type"].get<std::string>(), "Bearer");
    BOOST_CHECK_EQUAL(loginJson["expires_at"].get<std::string>(), timestamp);
    BOOST_CHECK(login.accessToken.view().empty());
    auto badUser = user(); badUser.createdAt = UtcTime(std::chrono::microseconds(std::numeric_limits<std::int64_t>::max()));
    BOOST_CHECK(!registeredUserResponse(badUser, requestId()));
    BOOST_CHECK(!loginResponse({SecretToken::session(token).value(), timeValue(), badUser}, requestId()));
    BOOST_CHECK(!loginResponse({SecretToken::invite(inviteToken).value(), timeValue(), user()}, requestId()));
}
BOOST_AUTO_TEST_CASE(inviteCreationListingAndEmptyResponses) {
    auto created = createdInviteResponse({invite(), SecretToken::invite(inviteToken).value()}, requestId());
    const auto json = body(created);
    headers(created.value(), 201);
    BOOST_CHECK_EQUAL(json.size(), 2);
    BOOST_CHECK_EQUAL(json["token"].get<std::string>(), inviteToken);
    BOOST_CHECK_EQUAL(json["invite"].size(), 7);
    BOOST_CHECK(json["invite"]["max_uses"].is_number_integer());
    BOOST_CHECK_EQUAL(json["invite"]["used_count"].get<int>(), 2);
    BOOST_CHECK(json["invite"]["revoked_at"].is_null());
    auto info = invite(); info.revokedAt = timeValue();
    auto page = invitesResponse({{info}, inviteCursor()}, requestId());
    const auto listed = body(page);
    headers(page.value(), 200);
    BOOST_CHECK_EQUAL(listed["items"][0]["revoked_at"].get<std::string>(), timestamp);
    BOOST_CHECK(page.value().body.find(inviteToken) == std::string::npos);
    BOOST_CHECK(!listed["items"][0].contains("token"));
    BOOST_CHECK(!listed["items"][0].contains("digest"));
    BOOST_CHECK(std::holds_alternative<InviteCursor>(decodeInviteCursor(listed["next_cursor"].get<std::string>())));
    const auto empty = body(invitesResponse({}, requestId()));
    BOOST_CHECK(empty["items"].is_array() && empty["items"].empty());
    BOOST_CHECK(empty["next_cursor"].is_null());
    BOOST_CHECK(!invitesResponse({{}, inviteCursor()}, requestId()));
    auto mismatched = inviteCursor(); mismatched.position.createdAt += std::chrono::seconds(1);
    BOOST_CHECK(!invitesResponse({{info}, mismatched}, requestId()));
    BOOST_CHECK(!createdInviteResponse({info, SecretToken::session(token).value()}, requestId()));
    auto output = noContentResponse(requestId());
    headers(output, 204);
    BOOST_CHECK(output.body.empty());
    BOOST_CHECK(!output.headers.headers().contains("Content-Type"));
    Buffer wire;
    BOOST_REQUIRE(output.buffered(false).appendToBuffer(&wire, HttpRequest::kHttp11, HttpRequest::kDelete));
    const auto bytes = wire.retrieveAllAsString();
    BOOST_CHECK(bytes.find("HTTP/1.1 204 No Content\r\n") == 0);
    BOOST_CHECK(bytes.substr(bytes.find("\r\n\r\n") + 4).empty());
}
BOOST_AUTO_TEST_CASE(nodeKindsEtagsAndLargeDecimalValues) {
    auto value = node();
    auto output = nodeResponse(value, requestId());
    auto json = body(output);
    headers(output.value(), 200);
    BOOST_CHECK_EQUAL(json.size(), 9);
    BOOST_CHECK_EQUAL(json["kind"].get<std::string>(), "directory");
    BOOST_CHECK(!json.contains("size") && !json.contains("sha256"));
    BOOST_CHECK_EQUAL(json["etag"].get<std::string>(), output.value().headers.headers().getSingle("ETag").value);
    value.metadata.parentId.reset(); value.metadata.name.clear();
    json = body(nodeResponse(value, requestId()));
    BOOST_CHECK(json["parent_id"].is_null());
    BOOST_CHECK_EQUAL(json["name"].get<std::string>(), "");
    value.metadata.name = "root";
    BOOST_CHECK(!nodeResponse(value, requestId()));
    value = node(); value.content = FileContentMetadata{ByteCount::from(std::numeric_limits<std::int64_t>::max()).value(), {0xab, 0xcd, 0xef}};
    json = body(nodeResponse(value, requestId()));
    BOOST_CHECK_EQUAL(json.size(), 11);
    BOOST_CHECK_EQUAL(json["kind"].get<std::string>(), "file");
    BOOST_CHECK_EQUAL(json["size"].get<std::string>(), "9223372036854775807");
    BOOST_CHECK_EQUAL(json["sha256"].get<std::string>(), "abcdef" + std::string(58, '0'));
    value.metadata.parentId.reset(); value.metadata.name.clear();
    BOOST_CHECK(!nodeResponse(value, requestId()));
    value = node(); value.metadata.name = std::string(256, 'x');
    BOOST_CHECK(!nodeResponse(value, requestId()));
}
BOOST_AUTO_TEST_CASE(spaceQuotasAreDecimalAndOwnershipShapeIsChecked) {
    const QuotaDelta limit{ByteCount::from(std::numeric_limits<std::int64_t>::max()).value(), NodeCount::from(100).value()};
    const QuotaDelta zero{ByteCount::from(0).value(), NodeCount::from(0).value()};
    const QuotaSnapshot quota{{limit, zero, zero}, Revision::from(1).value()};
    const auto privateId = SpaceId::parse("10000000-0000-4000-8000-000000000002").value();
    std::vector<SpaceView> spaces{
        {{privateId, SpaceKind::Private, UserId::parse(userId).value(), NodeId::parse(parentId).value()}, quota, std::nullopt},
        {{SpaceId::parse(spaceId).value(), SpaceKind::Public, std::nullopt, NodeId::parse(nodeId).value()}, quota, quota}};
    const auto output = spacesResponse(spaces, requestId());
    const auto json = body(output);
    headers(output.value(), 200);
    BOOST_CHECK(!output.value().headers.headers().contains("ETag"));
    BOOST_CHECK_EQUAL(json["items"].size(), 2);
    BOOST_CHECK_EQUAL(json["items"][0]["id"].get<std::string>(), spaceId);
    BOOST_CHECK(json["items"][0]["owner_id"].is_null());
    BOOST_CHECK(json["items"][0]["my_contribution_quota"].is_object());
    BOOST_CHECK_EQUAL(json["items"][1]["owner_id"].get<std::string>(), userId);
    BOOST_CHECK(json["items"][1]["my_contribution_quota"].is_null());
    BOOST_CHECK_EQUAL(json["items"][0]["quota"]["limit"]["bytes"].get<std::string>(), "9223372036854775807");
    BOOST_CHECK_EQUAL(json["items"][0]["quota"]["used"]["bytes"].get<std::string>(), "0");
    BOOST_CHECK_EQUAL(json["items"][0]["quota"]["revision"].get<std::string>(), "1");
    spaces[1].myContribution.reset();
    BOOST_CHECK(!spacesResponse(spaces, requestId()));
    spaces[1].myContribution = quota; spaces[0].space.ownerId.reset();
    BOOST_CHECK(!spacesResponse(spaces, requestId()));
    spaces.clear();
    BOOST_CHECK(!spacesResponse(spaces, requestId()));
}
BOOST_AUTO_TEST_CASE(pageSizeAndResponseStorageBounds) {
    const auto empty = body(childrenResponse({}, requestId()));
    BOOST_CHECK(empty["items"].is_array() && empty["items"].empty());
    BOOST_CHECK(empty["next_cursor"].is_null());
    auto page = Page<NodeDetails, ChildrenCursor>{{node()}, childrenCursor()};
    auto output = childrenResponse(page, requestId());
    auto json = body(output);
    BOOST_CHECK(std::holds_alternative<ChildrenCursor>(decodeChildrenCursor(json["next_cursor"].get<std::string>())));
    BOOST_CHECK(!output.value().headers.headers().contains("ETag"));
    page.next->parent = NodeId::parse(nodeId).value();
    BOOST_CHECK(!childrenResponse(page, requestId()));
    page.next.reset();
    page.items.clear();
    for (unsigned i = 0; i < 200; ++i) {
        auto value = node(); value.metadata.name = std::string(250, '"') + std::to_string(1000 + i);
        value.content = FileContentMetadata{ByteCount::from(1099511627776LL).value(), {}};
        page.items.push_back(std::move(value));
    }
    output = childrenResponse(page, requestId());
    BOOST_REQUIRE(output);
    BOOST_CHECK(output.value().body.size() > kMaxJsonBytes);
    BOOST_CHECK(output.value().body.size() < kMaxIdentityResponseBytes);
    BOOST_CHECK_EQUAL(body(output)["items"].size(), 200);
    page.items.push_back(node());
    BOOST_CHECK(!childrenResponse(page, requestId()));
    Page<InviteInfo, InviteCursor> invites;
    invites.items.assign(200, invite());
    BOOST_CHECK(invitesResponse(invites, requestId()));
    invites.items.push_back(invite());
    BOOST_CHECK(!invitesResponse(invites, requestId()));
}
BOOST_AUTO_TEST_CASE(newErrorsAndSubmissionRejectionsRemainDistinct) {
    struct Case { ErrorCode error; int status; const char* code; };
    for (const auto& entry : {
        Case{ErrorCode::LoginConflict, 409, "LOGIN_CONFLICT"}, Case{ErrorCode::InviteUnavailable, 409, "INVITE_UNAVAILABLE"},
        Case{ErrorCode::UserLimitReached, 409, "USER_LIMIT_REACHED"}, Case{ErrorCode::Forbidden, 403, "FORBIDDEN"},
        Case{ErrorCode::PreconditionFailed, 412, "PRECONDITION_FAILED"}, Case{ErrorCode::PreconditionRequired, 428, "PRECONDITION_REQUIRED"},
        Case{ErrorCode::RateLimited, 429, "RATE_LIMITED"}, Case{ErrorCode::SessionLimitReached, 409, "SESSION_LIMIT_REACHED"}, Case{ErrorCode::CommitOutcomeUnknown, 500, "COMMIT_OUTCOME_UNKNOWN"}}) {
        const auto output = errorResponse(protocolError(entry.error), requestId());
        headers(output, entry.status);
        const auto json = Json::parse(output.body);
        BOOST_CHECK_EQUAL(json["error"]["code"].get<std::string>(), entry.code);
        BOOST_CHECK_EQUAL(json["error"].size(), 4);
        BOOST_CHECK(json["error"]["details"].is_object() && json["error"]["details"].empty());
        BOOST_CHECK(output.body.find(token) == std::string::npos);
        BOOST_CHECK(output.body.find("password") == std::string::npos);
        BOOST_CHECK(output.body.find("rollback") == std::string::npos);
        Buffer wire;
        BOOST_REQUIRE(output.buffered(false).appendToBuffer(&wire, HttpRequest::kHttp11, HttpRequest::kPost));
        BOOST_CHECK(wire.retrieveAllAsString().find("HTTP/1.1 " + std::to_string(entry.status) + " ") == 0);
    }
    for (const auto& entry : {std::pair{SubmitError::RateLimited, 429}, std::pair{SubmitError::QueueFull, 503}, std::pair{SubmitError::Stopping, 503}, std::pair{SubmitError::InternalError, 500}}) {
        const auto output = errorResponse(protocolError(entry.first), requestId());
        headers(output, entry.second);
        BOOST_CHECK_EQUAL(Json::parse(output.body)["error"]["code"].get<std::string>(), entry.second == 429 ? "RATE_LIMITED" : entry.second == 503 ? "TEMPORARILY_UNAVAILABLE" : "INTERNAL_ERROR");
    }
    const auto unauthenticated = errorResponse(protocolError(ErrorCode::Unauthenticated), requestId());
    BOOST_CHECK_EQUAL(unauthenticated.headers.headers().getSingle("WWW-Authenticate").value, "Bearer realm=\"anof\"");
}
