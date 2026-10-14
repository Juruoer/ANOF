#include <anof/transport/http/internal/FlatJson.h>
#include <limits>

namespace anof::http::internal {
void clearText(std::string& text) noexcept {
    volatile char* bytes = text.data();
    for (std::size_t i = 0; i < text.size(); ++i) bytes[i] = 0;
    text.clear();
}
FlatJson::~FlatJson() {
    for (auto& field : fields_) clearText(field.text);
}
std::optional<ProtocolError> FlatJson::parse(std::string_view body, std::size_t limit) {
    if (body.size() > limit) return ProtocolError::RequestTooLarge;
    if (!nlohmann::json::sax_parse(body.begin(), body.end(), this) || !ended_) return ProtocolError::InvalidArgument;
    for (const auto& field : fields_) {
        if (field.required && !field.seen) return ProtocolError::InvalidArgument;
    }
    return std::nullopt;
}
bool FlatJson::start_object(std::size_t) {
    if (started_) return false;
    started_ = true;
    return true;
}
bool FlatJson::end_object() {
    if (current_) return false;
    ended_ = true;
    return true;
}
bool FlatJson::key(string_t& value) {
    for (auto& field : fields_) {
        if (field.name != value) continue;
        if (field.seen) return false;
        field.seen = true;
        current_ = &field;
        return true;
    }
    return false;
}
bool FlatJson::string(string_t& value) {
    const bool valid = current_ && current_->type == FieldType::Text;
    if (valid) current_->text = value;
    clearText(value);
    current_ = nullptr;
    return valid;
}
bool FlatJson::boolean(bool value) {
    if (!current_ || current_->type != FieldType::Boolean) return false;
    current_->boolean = value;
    current_ = nullptr;
    return true;
}
bool FlatJson::number_integer(number_integer_t value) {
    if (!current_ || current_->type != FieldType::Integer || value < 0) return false;
    current_->integer = value;
    current_ = nullptr;
    return true;
}
bool FlatJson::number_unsigned(number_unsigned_t value) {
    if (value > static_cast<number_unsigned_t>(std::numeric_limits<std::int64_t>::max())) return false;
    return number_integer(static_cast<number_integer_t>(value));
}
}  // namespace anof::http::internal
