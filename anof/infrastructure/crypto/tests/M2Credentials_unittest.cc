#define BOOST_TEST_MODULE AnofM2Credentials
#include <anof/infrastructure/crypto/SodiumCredentialIssuer.h>
#include <anof/infrastructure/crypto/SodiumIdentityIdGenerator.h>
#include <anof/infrastructure/crypto/SodiumInviteTokenCodec.h>
#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>
#include <anof/infrastructure/crypto/internal/RandomUuid.h>
#include <anof/infrastructure/crypto/internal/SecretBuffer.h>
#include <sodium.h>

#include <boost/test/unit_test.hpp>
#include <set>
#include <type_traits>

namespace anof::infrastructure::crypto {
namespace {
template <typename T>
void errorIs(const Result<T>& result, ErrorCode code) {
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == code);
}
template <typename T>
void uuid(const T& id) {
    const auto text = id.toString();
    BOOST_CHECK_EQUAL(text.size(), 36);
    BOOST_CHECK(text[14] == '4');
    BOOST_CHECK(text[19] == '8' || text[19] == '9' || text[19] == 'a' || text[19] == 'b');
    BOOST_REQUIRE(T::parse(text));
}
}  // namespace

BOOST_AUTO_TEST_CASE(issued_credentials_round_trip_and_hash_only_random_bytes) {
    auto issuer = makeSodiumCredentialIssuer();
    auto sessions = makeSodiumSessionTokenCodec();
    auto invites = makeSodiumInviteTokenCodec();
    BOOST_REQUIRE(issuer && issuer.value() && sessions && invites);
    std::set<std::array<unsigned char, 32>> digests;
    for (int i = 0; i < 16; ++i) {
        auto session = issuer.value()->issueSession();
        auto invite = issuer.value()->issueInvite();
        BOOST_REQUIRE(session && invite);
        BOOST_CHECK_EQUAL(session.value().text.view().size(), 43);
        BOOST_CHECK_EQUAL(invite.value().text.view().size(), 55);
        auto bearer = sessions.value()->digestBearer(session.value().text.view());
        auto invitation = InviteTokenText::parse(invite.value().text.view());
        BOOST_REQUIRE(bearer && invitation);
        auto digest = invites.value()->digest(invitation.value());
        BOOST_REQUIRE(digest);
        BOOST_CHECK(bearer.value() == session.value().digest);
        BOOST_CHECK(digest.value() == invite.value().digest);
        BOOST_CHECK(digests.insert(session.value().digest.bytes()).second);
        BOOST_CHECK(digests.insert(invite.value().digest.bytes()).second);
        detail::SecretBuffer<unsigned char, 32> decoded, expected;
        auto payload = invite.value().text.view().substr(12);
        std::size_t size = 0;
        BOOST_REQUIRE_EQUAL(sodium_base642bin(decoded.bytes.data(), decoded.bytes.size(), payload.data(), payload.size(), nullptr, &size, nullptr,
                                              sodium_base64_VARIANT_URLSAFE_NO_PADDING),
                            0);
        BOOST_CHECK_EQUAL(size, 32);
        BOOST_REQUIRE_EQUAL(crypto_hash_sha256(expected.bytes.data(), decoded.bytes.data(), size), 0);
        BOOST_CHECK(expected.bytes == invite.value().digest.bytes());
        errorIs(sessions.value()->digestBearer(invite.value().text.view()), ErrorCode::Unauthenticated);
    }
}

BOOST_AUTO_TEST_CASE(invite_fixed_digest_excludes_prefix_and_digest_types_are_distinct) {
    static_assert(!std::is_same_v<InviteDigest, SessionDigest>);
    static_assert(!std::is_convertible_v<InviteDigest, SessionDigest>);
    static_assert(!std::is_copy_constructible_v<SecretToken>);
    auto codec = makeSodiumInviteTokenCodec();
    BOOST_REQUIRE(codec);
    auto text = InviteTokenText::parse("anof_invite_" + std::string(43, 'A'));
    BOOST_REQUIRE(text);
    auto digest = codec.value()->digest(text.value());
    BOOST_REQUIRE(digest);
    // SHA-256(32 zero bytes)，独立固定答案。
    const InviteDigest::Bytes expected{ 0x66, 0x68, 0x7a, 0xad, 0xf8, 0x62, 0xbd, 0x77, 0x6c, 0x8f, 0xc1, 0x8b, 0x8e, 0x9f, 0x8e, 0x20,
                                        0x08, 0x97, 0x14, 0x85, 0x6e, 0xe2, 0x33, 0xb3, 0x90, 0x2a, 0x59, 0x1d, 0x0d, 0x5f, 0x29, 0x25 };
    BOOST_CHECK(digest.value().bytes() == expected);
}

BOOST_AUTO_TEST_CASE(invite_canonical_boundaries_and_moved_from_value) {
    auto codec = makeSodiumInviteTokenCodec();
    BOOST_REQUIRE(codec);
    const std::string valid = "anof_invite_" + std::string(43, 'A');
    for (const auto& text : { std::string{}, std::string(43, 'A'), valid + "=", valid + " ", " " + valid,
                              valid.substr(0, 54), valid + "A", std::string("ANOF_invite_") + std::string(43, 'A') }) {
        errorIs(InviteTokenText::parse(text), ErrorCode::InvalidArgument);
    }
    for (char c : std::string("=+/ \n\0", 6)) {
        auto text = valid;
        text[20] = c;
        errorIs(InviteTokenText::parse(text), ErrorCode::InvalidArgument);
    }
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (std::size_t i = 0; i < alphabet.size(); ++i) {
        auto text = valid;
        text.back() = alphabet[i];
        auto parsed = InviteTokenText::parse(text);
        if (i % 4)
            errorIs(parsed, ErrorCode::InvalidArgument);
        else {
            BOOST_REQUIRE(parsed);
            BOOST_REQUIRE(codec.value()->digest(parsed.value()));
        }
    }
    auto parsed = InviteTokenText::parse(valid);
    BOOST_REQUIRE(parsed);
    auto moved = std::move(parsed).value();
    errorIs(codec.value()->digest(parsed.value()), ErrorCode::InvalidArgument);
    BOOST_REQUIRE(codec.value()->digest(moved));
}

BOOST_AUTO_TEST_CASE(secret_outputs_survive_issuer_and_are_move_only) {
    auto issuer = makeSodiumCredentialIssuer();
    BOOST_REQUIRE(issuer);
    auto token = issuer.value()->issueInvite();
    BOOST_REQUIRE(token);
    issuer.value().reset();
    auto owned = std::move(token.value().text);
    BOOST_CHECK(token.value().text.view().empty());
    auto parsed = InviteTokenText::parse(owned.view());
    BOOST_REQUIRE(parsed);
    auto codec = makeSodiumInviteTokenCodec();
    BOOST_REQUIRE(codec);
    BOOST_CHECK(codec.value()->digest(parsed.value()).value() == token.value().digest);
}

BOOST_AUTO_TEST_CASE(identity_ids_are_typed_fresh_uuid_v4_values) {
    auto generator = makeSodiumIdentityIdGenerator();
    BOOST_REQUIRE(generator && generator.value());
    std::set<std::string> seen;
    for (int i = 0; i < 16; ++i) {
        auto check = [&](const auto& result) {
            BOOST_REQUIRE(result);
            uuid(result.value());
            BOOST_CHECK(seen.insert(result.value().toString()).second);
        };
        check(generator.value()->nextUser());
        check(generator.value()->nextSpace());
        check(generator.value()->nextNode());
        check(generator.value()->nextInvite());
        check(generator.value()->nextInstance());
        check(generator.value()->nextOperation());
    }
}

BOOST_AUTO_TEST_CASE(shared_uuid_formatter_preserves_random_bits_for_identity_types) {
    std::array<unsigned char, 16> bytes{};
    bytes.fill(0xff);
    auto check = [](const auto& result) {
        BOOST_REQUIRE(result);
        uuid(result.value());
        BOOST_CHECK(result.value().toString() == "ffffffff-ffff-4fff-bfff-ffffffffffff");
    };
    check(detail::uuidFromRandomBytes<UserId>(bytes));
    check(detail::uuidFromRandomBytes<SpaceId>(bytes));
    check(detail::uuidFromRandomBytes<NodeId>(bytes));
    check(detail::uuidFromRandomBytes<InviteId>(bytes));
    check(detail::uuidFromRandomBytes<InstanceId>(bytes));
    check(detail::uuidFromRandomBytes<OperationId>(bytes));
    auto zero = detail::uuidFromRandomBytes<UserId>({});
    BOOST_REQUIRE(zero);
    BOOST_CHECK(zero.value().toString() == "00000000-0000-4000-8000-000000000000");
}
}  // namespace anof::infrastructure::crypto
