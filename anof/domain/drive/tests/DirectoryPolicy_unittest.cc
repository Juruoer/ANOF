#define BOOST_TEST_MODULE AnofDirectoryPolicy
#include <anof/domain/drive/DirectoryPolicy.h>

#include <boost/test/unit_test.hpp>
#include <iomanip>
#include <sstream>

using namespace anof;

namespace {
template <typename T>
T id(unsigned int n) {
    std::ostringstream text;
    text << "00000000-0000-4000-8000-" << std::hex << std::setw(12) << std::setfill('0') << n;
    return T::parse(text.str()).value();
}
const auto owner = id<UserId>(1);
const Space space{ id<SpaceId>(1), SpaceKind::Private, owner, id<NodeId>(1) };
DirectoryPath path(std::size_t depth) {
    DirectoryPath result;
    for (std::size_t i = 0; i <= depth; ++i) {
        result.push_back({ id<NodeId>(static_cast<unsigned int>(i + 1)), space.id,
                           i == 0 ? std::nullopt : std::optional<NodeId>(result.back().id),
                           "directory", owner, Revision{}, UtcTime{}, UtcTime{} });
    }
    return result;
}
}  // namespace

BOOST_AUTO_TEST_CASE(utf8_byte_limits_and_no_normalization) {
    for (const std::string& name : { std::string("文件"), std::string(" A "), std::string("a"),
                                     std::string("A"), std::string("é"), std::string("e\xcc\x81"),
                                     std::string("\xf4\x8f\xbf\xbf"), std::string(255, 'x') }) {
        BOOST_CHECK(validateDirectoryName(name));
    }
    std::string multibyte;
    for (int i = 0; i < 85; ++i) multibyte += "文";
    BOOST_CHECK(validateDirectoryName(multibyte));
    BOOST_CHECK(!validateDirectoryName(multibyte + "a"));
}

BOOST_AUTO_TEST_CASE(rejects_invalid_encoding_controls_and_path_separators) {
    const std::vector<std::string> invalid{
        "", ".", "..", "a/b", "a\\b", std::string("a\0b", 3), "\x1f", "\x7f", "\xc2\x80", "\xc2\x9f",
        "\x80", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80",
        "\xe4\xb8", "\xe4\x41\x80", std::string(256, 'x')
    };
    for (const auto& name : invalid) {
        auto result = validateDirectoryName(name);
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::InvalidName);
    }
}

BOOST_AUTO_TEST_CASE(private_visibility_and_public_contribution) {
    const Principal member{ owner, UserRole::Member, AuthEpoch{} };
    const Principal admin{ id<UserId>(2), UserRole::Administrator, AuthEpoch{} };
    BOOST_CHECK(requireCreatePermission(member, space));
    auto hidden = requireCreatePermission(admin, space);
    BOOST_REQUIRE(!hidden);
    BOOST_CHECK(hidden.error().code == ErrorCode::NotFound);
    const Space publicSpace{ space.id, SpaceKind::Public, std::nullopt, space.rootNodeId };
    for (const auto& principal : { member, admin }) {
        auto scopes = directoryQuotaScopes(principal, publicSpace);
        BOOST_REQUIRE(scopes);
        BOOST_REQUIRE_EQUAL(scopes.value().size(), 2U);
        BOOST_CHECK(std::get<PublicContributionQuota>(scopes.value()[1]).userId == principal.userId);
    }
    BOOST_TEST(directoryQuotaScopes(member, space).value().size() == 1U);
    auto malformed = space;
    malformed.ownerId.reset();
    BOOST_CHECK(!requireCreatePermission(member, malformed));
}

BOOST_AUTO_TEST_CASE(depth_and_corrupt_paths) {
    auto valid = path(127);
    BOOST_CHECK(validateDirectoryParent(space, valid.back().id, valid));
    auto tooDeep = path(128);
    auto denied = validateDirectoryParent(space, tooDeep.back().id, tooDeep);
    BOOST_REQUIRE(!denied);
    BOOST_CHECK(denied.error().code == ErrorCode::DepthLimitExceeded);
    BOOST_CHECK(!validateDirectoryParent(space, space.rootNodeId, {}));
    valid[1].parentId = id<NodeId>(999);
    BOOST_CHECK(!validateDirectoryParent(space, valid.back().id, valid));
    valid = path(2);
    valid.back().id = valid.front().id;
    BOOST_CHECK(!validateDirectoryParent(space, valid.back().id, valid));
    valid = path(2);
    valid[1].spaceId = id<SpaceId>(2);
    BOOST_CHECK(!validateDirectoryParent(space, valid.back().id, valid));
}
