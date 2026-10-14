#include <anof/transport/http/AccountRequest.h>
#include <anof/transport/http/internal/FlatJson.h>

#include <charconv>

namespace anof::http {
namespace {
// from_chars 负责溢出检查；语法检查排除符号、空白和前导零。
std::optional<std::int64_t> decimal(std::string_view text) {
    if (text.empty() || (text.size() > 1 && text.front() == '0') || text.find_first_not_of("0123456789") != std::string_view::npos) return std::nullopt;
    std::int64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}
template <typename IdType>
std::optional<IdType> takeId(std::string_view& text) {
    if (text.size() < 37 || text[36] != '-') return std::nullopt;
    const auto raw = text.substr(0, 36);
    const auto id = IdType::parse(raw);
    if (!id || id.value().toString() != raw) return std::nullopt;
    text.remove_prefix(37);
    return id.value();
}
// 只允许 {limit:{bytes:string,nodes:string}}，不建立通用 DOM；两层分别记录重复键。
class QuotaJson final : public nlohmann::json_sax<nlohmann::json> {
public:
    std::optional<std::int64_t> bytes;
    std::optional<std::int64_t> nodes;
    bool ended = false;
    bool null() override { return false; }
    bool boolean(bool) override { return false; }
    bool number_integer(number_integer_t) override { return false; }
    bool number_unsigned(number_unsigned_t) override { return false; }
    bool number_float(number_float_t, const string_t&) override { return false; }
    bool binary(binary_t&) override { return false; }
    bool start_array(std::size_t) override { return false; }
    bool end_array() override { return false; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
    bool start_object(std::size_t) override {
        if (ended || depth_ >= 2 || (depth_ == 1 && field_ != 1)) return false;
        ++depth_; field_ = 0;
        return true;
    }
    bool key(string_t& key) override {
        if (depth_ == 1 && key == "limit" && !limitSeen_) {
            limitSeen_ = true; field_ = 1; return true;
        }
        if (depth_ == 2 && key == "bytes" && !bytes) { field_ = 2; return true; }
        if (depth_ == 2 && key == "nodes" && !nodes) { field_ = 3; return true; }
        return false;
    }
    bool string(string_t& value) override {
        const auto parsed = decimal(value);
        if (depth_ != 2 || !parsed) return false;
        if (field_ == 2) bytes = *parsed;
        else if (field_ == 3) nodes = *parsed;
        else return false;
        field_ = 0; return true;
    }
    bool end_object() override {
        if (field_ || !bytes || !nodes || depth_ == 0) return false;
        --depth_; ended = depth_ == 0; return true;
    }
private:
    unsigned depth_ = 0;
    unsigned field_ = 0;
    bool limitSeen_ = false;
};
}  // namespace

Parsed<AccountCondition> parseAccountCondition(const muduox::net::HttpHeaders& headers) {
    using Status = muduox::net::HttpHeaders::SingleStatus;
    const auto header = headers.getSingle("if-match");
    if (header.status == Status::Missing) return ProtocolError::PreconditionRequired;
    if (header.status != Status::Unique) return ProtocolError::InvalidArgument;
    auto tag = header.value;
    while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t')) tag.remove_prefix(1);
    while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t')) tag.remove_suffix(1);
    if (tag.size() < 2 || tag.front() != '"' || tag.back() != '"') return ProtocolError::InvalidArgument;
    tag.remove_prefix(1); tag.remove_suffix(1);
    if (tag.substr(0, 5) == "user-") {
        tag.remove_prefix(5);
        const auto id = takeId<UserId>(tag);
        const auto revision = decimal(tag);
        if (!id || !revision || *revision == 0) return ProtocolError::InvalidArgument;
        return AccountCondition{UserCondition{*id, Revision::from(*revision).value()}};
    }
    const bool contribution = tag.substr(0, 19) == "quota-contribution-";
    if (!contribution && tag.substr(0, 12) != "quota-space-") return ProtocolError::InvalidArgument;
    tag.remove_prefix(contribution ? 19 : 12);
    const auto space = takeId<SpaceId>(tag);
    if (!space) return ProtocolError::InvalidArgument;
    QuotaScope scope = SpaceQuota{*space};
    if (contribution) {
        const auto user = takeId<UserId>(tag);
        if (!user) return ProtocolError::InvalidArgument;
        scope = PublicContributionQuota{*space, *user};
    }
    const auto revision = decimal(tag);
    if (!revision || *revision == 0) return ProtocolError::InvalidArgument;
    return AccountCondition{QuotaCondition{scope, Revision::from(*revision).value()}};
}
Parsed<GetUserCommand> parseGetUser(std::string_view id) {
    const auto user = UserId::parse(id);
    if (!user) return ProtocolError::InvalidArgument;
    return GetUserCommand{user.value()};
}
Parsed<QuotaScope> parseQuotaScope(std::string_view spaceId, std::optional<std::string_view> userId) {
    const auto space = SpaceId::parse(spaceId);
    if (!space) return ProtocolError::InvalidArgument;
    if (!userId) return QuotaScope{SpaceQuota{space.value()}};
    const auto user = UserId::parse(*userId);
    if (!user) return ProtocolError::InvalidArgument;
    return QuotaScope{PublicContributionQuota{space.value(), user.value()}};
}
Parsed<UpdateDisplayNameCommand> parseUpdateDisplayNameJson(std::string_view body, AccountCondition expected) {
    internal::FlatJson json{{"display_name"}};
    if (const auto error = json.parse(body)) return *error;
    auto name = DisplayName::parse(json[0].text);
    if (!name) return ProtocolError::InvalidArgument;
    return UpdateDisplayNameCommand{std::move(expected), std::move(name).value()};
}
Parsed<ChangePasswordCommand> parseChangePasswordJson(std::string_view body, AccountCondition expected, AuthenticationSource source) {
    internal::FlatJson json{{"current_password"}, {"new_password"}};
    if (const auto error = json.parse(body)) return *error;
    auto current = PasswordInput::parse(json[0].text);
    auto next = PasswordInput::parse(json[1].text);
    if (!current || !next) return ProtocolError::InvalidArgument;
    return ChangePasswordCommand{std::move(expected), std::move(current).value(), std::move(next).value(), source};
}
Parsed<SetUserEnabledCommand> parseSetUserEnabledJson(std::string_view body, UserId id, AccountCondition expected) {
    internal::FlatJson json{{"enabled", internal::FieldType::Boolean}};
    if (const auto error = json.parse(body)) return *error;
    return SetUserEnabledCommand{id, std::move(expected), json[0].boolean};
}
Parsed<SetQuotaLimitCommand> parseSetQuotaLimitJson(std::string_view body, QuotaScope scope, AccountCondition expected) {
    if (body.size() > kMaxJsonBytes) return ProtocolError::RequestTooLarge;
    QuotaJson json;
    if (!nlohmann::json::sax_parse(body.begin(), body.end(), &json) || !json.ended) return ProtocolError::InvalidArgument;
    return SetQuotaLimitCommand{std::move(scope), std::move(expected), {ByteCount::from(*json.bytes).value(), NodeCount::from(*json.nodes).value()}};
}
}  // namespace anof::http
