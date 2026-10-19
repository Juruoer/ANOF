#include <anof/infrastructure/system/internal/InitializationFiles.h>
#include <nlohmann/json.hpp>
#include <cerrno>
#include <optional>
#include <new>

namespace anof::infrastructure::system::detail {
namespace {
class MarkerSax final : public nlohmann::json_sax<nlohmann::json> {
public:
    explicit MarkerSax(bool pending) : pending_(pending) {}
    bool null() override { return false; }
    bool boolean(bool) override { return false; }
    bool number_integer(number_integer_t value) override { return number(value >= 0 ? static_cast<std::uint64_t>(value) : 0); }
    bool number_unsigned(number_unsigned_t value) override { return number(value); }
    bool number_float(number_float_t, const string_t&) override { return false; }
    bool string(string_t& value) override {
        if (field_ == 1) {
            auto id = InstanceId::parse(value);
            if (!id || id.value().toString() != value) return false;
            instance = id.value();
        } else if (field_ == 2) {
            auto id = OperationId::parse(value);
            if (!id || id.value().toString() != value) return false;
            operation = id.value();
        } else return false;
        field_ = 0;
        return true;
    }
    bool binary(binary_t&) override { return false; }
    bool start_object(std::size_t) override { if (started_) return false; started_ = true; return true; }
    bool key(string_t& key) override {
        if (field_ || ended_) return false;
        if (key == "instance_id") field_ = 1;
        else if (pending_ && key == "operation_id") field_ = 2;
        else if (pending_ && key == "version") field_ = 3;
        else if (pending_ && key == "target_schema") field_ = 4;
        else if ((pending_ && key == "target_format") || (!pending_ && key == "format_version")) field_ = 5;
        else return false;
        const unsigned mask = 1U << field_;
        if (seen_ & mask) return false;
        seen_ |= mask;
        return true;
    }
    bool end_object() override {
        ended_ = started_ && field_ == 0 && seen_ == (pending_ ? 62U : 34U);
        return ended_;
    }
    bool start_array(std::size_t) override { return false; }
    bool end_array() override { return false; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
    bool unsupported = false;
    std::optional<InstanceId> instance;
    std::optional<OperationId> operation;
private:
    bool number(std::uint64_t value) {
        if (field_ < 3 || field_ > 5) return false;
        if (value != (field_ == 3 ? 1U : static_cast<unsigned>(field_ == 4 ? InitializationIdentity::schemaVersion : InitializationIdentity::formatVersion))) unsupported = true;
        field_ = 0;
        return true;
    }
    bool pending_;
    bool started_ = false, ended_ = false;
    unsigned field_ = 0, seen_ = 0;
};
InitResult<void> parse(std::string_view text, MarkerSax& sax, InitStage stage) {
    if (text.size() > 4096 || text.substr(0, 3) == "\xef\xbb\xbf") return InitFailure{ InitError::IntegrityError, stage };
    try {
        if (!nlohmann::json::sax_parse(text, &sax)) return InitFailure{ InitError::IntegrityError, stage };
        if (sax.unsupported) return InitFailure{ InitError::UnsupportedFormat, stage };
        return {};
    } catch (const std::bad_alloc&) { return InitFailure{ InitError::InternalError, stage }; }
    catch (const nlohmann::json::exception&) { return InitFailure{ InitError::IntegrityError, stage }; }
}
}  // namespace
InitResult<InitializationIdentity> parsePending(std::string_view text, InitStage stage) {
    MarkerSax sax(true);
    auto valid = parse(text, sax, stage);
    if (!valid) return valid.error();
    return InitializationIdentity{ *sax.instance, *sax.operation };
}
InitResult<InstanceId> parseFormat(std::string_view text, InitStage stage) {
    MarkerSax sax(false);
    auto valid = parse(text, sax, stage);
    if (!valid) return valid.error();
    return *sax.instance;
}
std::string pendingText(const InitializationIdentity& identity) {
    return "{\"version\":1,\"operation_id\":\"" + identity.operationId.toString() + "\",\"instance_id\":\"" + identity.instanceId.toString() + "\",\"target_schema\":" + std::to_string(InitializationIdentity::schemaVersion) + ",\"target_format\":" + std::to_string(InitializationIdentity::formatVersion) + "}\n";
}
std::string formatText(const InitializationIdentity& identity) {
    return "{\"instance_id\":\"" + identity.instanceId.toString() + "\",\"format_version\":" + std::to_string(InitializationIdentity::formatVersion) + "}\n";
}
InitFailure initializationError(ErrorCode code, InitStage stage) {
    switch (code) {
    case ErrorCode::InvalidArgument: return { InitError::InvalidInput, stage };
    case ErrorCode::NotFound: return { InitError::NotFound, stage };
    case ErrorCode::IntegrityError: return { InitError::IntegrityError, stage };
    case ErrorCode::StorageFull: return { InitError::StorageFull, stage };
    default: return { InitError::IoError, stage };
    }
}
InitFailure initializationErrno(int error, InitStage stage) {
    switch (error) {
    case ENOSPC: case EDQUOT: return { InitError::StorageFull, stage };
    case ENOENT: return { InitError::NotFound, stage };
    case EEXIST: return { InitError::AlreadyExists, stage };
    case ELOOP: case ENOTDIR: case EISDIR: return { InitError::IntegrityError, stage };
    default: return { InitError::IoError, stage };
    }
}
}  // namespace anof::infrastructure::system::detail
