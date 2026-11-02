#pragma once

#include <anof/transport/http/JsonResponse.h>

#include <initializer_list>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace anof::http::internal {
// 仅清理本模块持有的字符串，不承诺清除调用者或 JSON lexer 内部副本。
void clearText(std::string& text) noexcept;
enum class FieldType { Text,
                       Integer,
                       Boolean };
struct Field {
    Field(std::string_view fieldName, FieldType fieldType = FieldType::Text, bool isRequired = true)
        : name(fieldName), type(fieldType), required(isRequired) {}
    std::string_view name;
    FieldType type = FieldType::Text;
    bool required = true;
    bool seen = false;
    std::string text;
    std::int64_t integer = 0;
    bool boolean = false;
};
// 用于平坦请求和游标；嵌套容器在起始事件拒绝，早于深度 32 上限。
class FlatJson final : public nlohmann::json_sax<nlohmann::json> {
public:
    explicit FlatJson(std::initializer_list<Field> fields) : fields_(fields) {}
    ~FlatJson() override;
    FlatJson(const FlatJson&) = delete;
    FlatJson& operator=(const FlatJson&) = delete;
    std::optional<ProtocolError> parse(std::string_view body, std::size_t limit = kMaxJsonBytes);
    const Field& operator[](std::size_t index) const { return fields_.at(index); }
    bool null() override { return false; }
    bool boolean(bool value) override;
    bool number_integer(number_integer_t value) override;
    bool number_unsigned(number_unsigned_t value) override;
    bool number_float(number_float_t, const string_t&) override { return false; }
    bool binary(binary_t&) override { return false; }
    bool start_array(std::size_t) override { return false; }
    bool end_array() override { return false; }
    bool start_object(std::size_t) override;
    bool end_object() override;
    bool key(string_t& value) override;
    bool string(string_t& value) override;
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }

private:
    std::vector<Field> fields_;
    Field* current_ = nullptr;
    bool started_ = false;
    bool ended_ = false;
};
}  // namespace anof::http::internal
