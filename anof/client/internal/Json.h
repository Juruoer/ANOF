#pragma once
#include <anof/client/ClientTypes.h>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace anof::client::internal {
using Json = nlohmann::json;
inline void wipe(std::string& text) noexcept { volatile char* p = text.data(); for (std::size_t i = 0; i < text.size(); ++i) p[i] = 0; }
inline void wipe(Json& value) noexcept {
    if (value.is_string()) wipe(value.get_ref<std::string&>());
    else if (value.is_structured()) for (auto& item : value) wipe(item);
}
struct InvalidJson {};
inline void require(bool condition) { if (!condition) throw InvalidJson{}; }
// SAX 构建时限制深度并检测所有重复键；失败时同样清理已构建的字符串值。
struct Document : nlohmann::json_sax<Json> {
    Json root;
    struct Frame { Json value; std::string key; std::set<std::string> keys; };
    std::vector<Frame> stack;
    bool hasRoot = false;
    ~Document() { wipe(root); for (auto& frame : stack) wipe(frame.value); }
    bool put(Json value) {
        if (stack.empty()) { if (hasRoot) { wipe(value); return false; } root = std::move(value); hasRoot = true; }
        else if (stack.back().value.is_array()) stack.back().value.push_back(std::move(value));
        else stack.back().value[stack.back().key] = std::move(value);
        return true;
    }
    bool null() override { return put(nullptr); }
    bool boolean(bool v) override { return put(v); }
    bool number_integer(number_integer_t v) override { return put(v); }
    bool number_unsigned(number_unsigned_t v) override { return put(v); }
    bool number_float(number_float_t v, const string_t&) override { return put(v); }
    bool string(string_t& v) override { const bool ok = put(v); wipe(v); return ok; }
    bool binary(binary_t&) override { return false; }
    bool start_object(std::size_t) override { return start(Json::object()); }
    bool start_array(std::size_t) override { return start(Json::array()); }
    bool start(Json value) { if (stack.size() >= 32) return false; stack.push_back({std::move(value), {}, {}}); return true; }
    bool key(string_t& v) override {
        if (stack.empty() || !stack.back().keys.insert(v).second) return false;
        stack.back().key = v; return true;
    }
    bool end_object() override { return end(); }
    bool end_array() override { return end(); }
    bool end() { auto value = std::move(stack.back().value); stack.pop_back(); return put(std::move(value)); }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
    void parse(std::string_view text) { require(Json::sax_parse(text.begin(), text.end(), this) && hasRoot && stack.empty()); }
};
inline const Json& field(const Json& j, const char* key) { require(j.is_object() && j.contains(key)); return j.at(key); }
inline const std::string& string(const Json& j) { require(j.is_string()); return j.get_ref<const std::string&>(); }
inline const std::string& string(const Json& j, const char* key) { return string(field(j, key)); }
inline bool boolean(const Json& j, const char* key) { const auto& v = field(j, key); require(v.is_boolean()); return v.get<bool>(); }
inline std::int64_t integer(const Json& j, std::int64_t min, std::int64_t max) {
    require(j.is_number_integer());
    if (j.is_number_unsigned()) require(j.get<std::uint64_t>() <= static_cast<std::uint64_t>(max));
    auto v = j.get<std::int64_t>(); require(v >= min && v <= max); return v;
}
inline std::int64_t decimal(std::string_view s, std::int64_t minimum = 0) {
    require(!s.empty() && (s.size() == 1 || s.front() != '0'));
    std::int64_t value = 0;
    for (char c : s) { require(c >= '0' && c <= '9' && value <= (INT64_MAX - (c - '0')) / 10); value = value * 10 + c - '0'; }
    require(value >= minimum); return value;
}
template<class T> T id(const Json& j) { const auto& s = string(j); auto value = T::parse(s); require(value && value.value().toString() == s); return std::move(value).value(); }
inline LoginName login(const Json& j) { const auto& s = string(j); auto v = LoginName::parse(s); require(v && v.value().value() == s); return std::move(v).value(); }
bool validTimestamp(std::string_view text) noexcept;
inline std::string time(const Json& j) { const auto& s = string(j); require(validTimestamp(s)); return s; }
inline void exactKeys(const Json& j, std::initializer_list<const char*> keys) { require(j.is_object() && j.size() == keys.size()); for (auto k : keys) require(j.contains(k)); }
inline void append(SecureBuffer& buffer, std::string_view value) { require(buffer.append(value)); }
// 秘密直接转义写入固定堆缓冲，不经过 string/JSON dump 的额外副本。
inline void quoted(SecureBuffer& buffer, std::string_view value) {
    append(buffer, "\"");
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { const char bytes[] = {'\\', static_cast<char>(c)}; append(buffer, {bytes, 2}); }
        else if (c < 32) { const char bytes[] = {'\\','u','0','0',hex[c >> 4],hex[c & 15]}; append(buffer, {bytes, 6}); }
        else { const char byte = static_cast<char>(c); append(buffer, {&byte, 1}); }
    }
    append(buffer, "\"");
}
}  // namespace anof::client::internal
