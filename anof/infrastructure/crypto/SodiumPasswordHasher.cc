#include <anof/infrastructure/crypto/SodiumPasswordHasher.h>
#include <anof/infrastructure/crypto/internal/PasswordWork.h>
#include <anof/infrastructure/crypto/internal/SecretBuffer.h>
#include <anof/infrastructure/crypto/internal/SodiumSupport.h>
#include <algorithm>
#include <new>

namespace anof::infrastructure::crypto {
namespace {
Result<EncodedPasswordHash> checkedEncoding(std::string_view text, PasswordHashLimits limits) {
    auto parsed = EncodedPasswordHash::parse(text, limits);
    if (!parsed) return parsed.error();
    // 领域值还允许其他盐/输出长度；适配器只支持 libsodium 的固定 PHC 形状。
    const auto saltStart = text.find('$', 16);
    const auto hashStart = saltStart == text.npos ? text.npos : text.find('$', saltStart + 1);
    if (saltStart == text.npos || hashStart == text.npos || hashStart - saltStart - 1 != 22 || text.size() - hashStart - 1 != 43) return ErrorCode::InvalidArgument;
    return std::move(parsed).value();
}
Result<void> decode(std::string_view text, unsigned char* output, std::size_t size) {
    std::size_t written = 0;
    const char* end = nullptr;
    if (sodium_base642bin(output, size, text.data(), text.size(), nullptr, &written, &end,
                         sodium_base64_VARIANT_ORIGINAL_NO_PADDING) != 0 || written != size || end != text.data() + text.size()) return ErrorCode::InvalidArgument;
    return {};
}
class SodiumPasswordHasher final : public PasswordHasher {
public:
    explicit SodiumPasswordHasher(SodiumPasswordPolicy policy) : policy_(policy) {}
    Result<EncodedPasswordHash> hash(const PasswordInput& password) override {
        return detail::hashPasswordWith(password, policy_, crypto_pwhash_argon2id_str);
    }
    Result<bool> verify(const PasswordInput& password, const EncodedPasswordHash& encoded) override {
        return detail::verifyPasswordWith(password, encoded, policy_, crypto_pwhash_argon2id);
    }
private:
    const SodiumPasswordPolicy policy_;
};
}  // namespace

Result<EncodedPasswordHash> detail::hashPasswordWith(const PasswordInput& password, const SodiumPasswordPolicy& policy, HashString hash) {
    auto valid = validateSodiumPasswordPolicy(policy);
    if (!valid) return valid.error();
    const auto input = password.view();
    if (input.size() < 12 || input.size() > 1024) return ErrorCode::InvalidArgument;
    if (!hash) return ErrorCode::InternalError;
    try {
        SecretBuffer<char, crypto_pwhash_argon2id_STRBYTES> output;
        const auto memory = static_cast<std::size_t>(policy.hashing.memoryKiB) * 1024;
        if (hash(output.bytes.data(), input.data(), input.size(), policy.hashing.iterations, memory) != 0) return ErrorCode::InternalError;
        const auto end = std::find(output.bytes.begin(), output.bytes.end(), '\0');
        if (end == output.bytes.end()) return ErrorCode::InternalError;
        auto encoded = checkedEncoding({ output.bytes.data(), static_cast<std::size_t>(end - output.bytes.begin()) }, policy.verification);
        if (!encoded) return ErrorCode::InternalError;
        const auto parameters = encoded.value().parameters();
        if (parameters.memoryKiB != policy.hashing.memoryKiB || parameters.iterations != policy.hashing.iterations || parameters.parallelism != 1) return ErrorCode::InternalError;
        return std::move(encoded).value();
    } catch (const std::bad_alloc&) { return ErrorCode::InternalError; }
}

Result<bool> detail::verifyPasswordWith(const PasswordInput& password, const EncodedPasswordHash& encoded,
                                       const SodiumPasswordPolicy& policy, DerivePassword derive) {
    auto valid = validateSodiumPasswordPolicy(policy);
    if (!valid) return valid.error();
    const auto input = password.view();
    if (input.size() < 12 || input.size() > 1024) return ErrorCode::InvalidArgument;
    try {
        auto parsed = checkedEncoding(encoded.view(), policy.verification);
        if (!parsed) return parsed.error();
        const auto text = parsed.value().view();
        const auto saltStart = text.find('$', 16) + 1;
        const auto hashStart = text.find('$', saltStart) + 1;
        SecretBuffer<unsigned char, 16> salt;
        SecretBuffer<unsigned char, 32> expected, actual;
        auto decoded = decode(text.substr(saltStart, 22), salt.bytes.data(), salt.bytes.size());
        if (!decoded) return decoded.error();
        decoded = decode(text.substr(hashStart), expected.bytes.data(), expected.bytes.size());
        if (!decoded) return decoded.error();
        if (!derive) return ErrorCode::InternalError;
        const auto parameters = parsed.value().parameters();
        // 不使用 str_verify 的统一 -1：计算失败与正常密码不匹配保持不同结果。
        if (derive(actual.bytes.data(), actual.bytes.size(), input.data(), input.size(), salt.bytes.data(),
                   parameters.iterations, static_cast<std::size_t>(parameters.memoryKiB) * 1024,
                   crypto_pwhash_argon2id_ALG_ARGON2ID13) != 0) return ErrorCode::InternalError;
        return sodium_memcmp(actual.bytes.data(), expected.bytes.data(), actual.bytes.size()) == 0;
    } catch (const std::bad_alloc&) { return ErrorCode::InternalError; }
}

Result<std::unique_ptr<PasswordHasher>> makeSodiumPasswordHasher(const SodiumPasswordPolicy& policy) {
    auto valid = validateSodiumPasswordPolicy(policy);
    if (!valid) return valid.error();
    auto initialized = detail::initializeSodium();
    if (!initialized) return initialized.error();
    try { return std::unique_ptr<PasswordHasher>(new SodiumPasswordHasher(policy)); }
    catch (const std::bad_alloc&) { return ErrorCode::InternalError; }
}
}  // namespace anof::infrastructure::crypto
