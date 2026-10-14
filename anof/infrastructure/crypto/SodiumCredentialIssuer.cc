#include <anof/infrastructure/crypto/SodiumCredentialIssuer.h>
#include <anof/infrastructure/crypto/internal/SecretBuffer.h>
#include <anof/infrastructure/crypto/internal/SodiumSupport.h>
#include <algorithm>
#include <new>
#include <type_traits>

namespace anof::infrastructure::crypto {
namespace {
class SodiumCredentialIssuer final : public CredentialIssuer {
public:
    Result<IssuedSessionToken> issueSession() override { return issue<false>(); }
    Result<IssuedInviteToken> issueInvite() override { return issue<true>(); }
private:
    template <bool Invite>
    auto issue() -> Result<std::conditional_t<Invite, IssuedInviteToken, IssuedSessionToken>> {
        using Digest = std::conditional_t<Invite, InviteDigest, SessionDigest>;
        try {
            detail::SecretBuffer<unsigned char, 32> random, digest;
            detail::SecretBuffer<char, 56> encoded;
            constexpr std::string_view prefix = "anof_invite_";
            constexpr std::size_t offset = Invite ? prefix.size() : 0;
            randombytes_buf(random.bytes.data(), random.bytes.size());
            if (crypto_hash_sha256(digest.bytes.data(), random.bytes.data(), random.bytes.size()) != 0) return ErrorCode::InternalError;
            if constexpr (Invite) std::copy(prefix.begin(), prefix.end(), encoded.bytes.begin());
            if (!sodium_bin2base64(encoded.bytes.data() + offset, encoded.bytes.size() - offset, random.bytes.data(), random.bytes.size(),
                                  sodium_base64_VARIANT_URLSAFE_NO_PADDING)) return ErrorCode::InternalError;
            const std::string_view text(encoded.bytes.data(), offset + 43);
            auto secret = Invite ? SecretToken::invite(text) : SecretToken::session(text);
            if (!secret) return ErrorCode::InternalError;
            using Issued = std::conditional_t<Invite, IssuedInviteToken, IssuedSessionToken>;
            return Issued{ std::move(secret).value(), Digest(digest.bytes) };
        } catch (const std::bad_alloc&) { return ErrorCode::InternalError; }
    }
};
}  // namespace
Result<std::unique_ptr<CredentialIssuer>> makeSodiumCredentialIssuer() {
    auto initialized = detail::initializeSodium();
    if (!initialized) return initialized.error();
    try { return std::unique_ptr<CredentialIssuer>(new SodiumCredentialIssuer); }
    catch (const std::bad_alloc&) { return ErrorCode::InternalError; }
}
}  // namespace anof::infrastructure::crypto
