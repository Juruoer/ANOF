#include <anof/transport/http/CreateDirectoryJson.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <nlohmann/json.hpp>

#include <array>

namespace anof::http {
namespace {
// 接口仅允许一层对象和三个字符串。遇到容器字段立即拒绝，比深度 32 上限更严格，
// 因而恶意嵌套不会建立 DOM 或递归积累；key 回调接收已解码的键。
class DirectorySax final : public nlohmann::json_sax<nlohmann::json> {
public:
    std::array<std::string, 3> values;
    std::array<bool, 3> seen{};
    bool started = false;
    bool ended = false;
    int field = -1;
    bool null() override { return false; }
    bool boolean(bool) override { return false; }
    bool number_integer(number_integer_t) override { return false; }
    bool number_unsigned(number_unsigned_t) override { return false; }
    bool number_float(number_float_t, const string_t&) override { return false; }
    bool binary(binary_t&) override { return false; }
    bool start_array(std::size_t) override { return false; }
    bool end_array() override { return false; }
    bool start_object(std::size_t) override {
        if (started) return false;
        started = true;
        return true;
    }
    bool key(string_t& key) override {
        if (key == "space_id") field = 0;
        else if (key == "parent_id") field = 1;
        else if (key == "name") field = 2;
        else return false;
        auto& present = seen[static_cast<std::size_t>(field)];
        if (present) return false;
        present = true;
        return true;
    }
    bool string(string_t& value) override {
        if (field < 0 || ended) return false;
        values[static_cast<std::size_t>(field)] = std::move(value);
        field = -1;
        return true;
    }
    bool end_object() override { ended = true; return true; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
};
}  // namespace

CreateDirectoryJsonResult parseCreateDirectoryJson(std::string_view body) {
    if (body.size() > kMaxJsonBytes) return ProtocolError::RequestTooLarge;
    DirectorySax sax;
    if (!nlohmann::json::sax_parse(body.begin(), body.end(), &sax) || !sax.ended || !sax.seen[0] || !sax.seen[1] || !sax.seen[2]) return ProtocolError::InvalidArgument;
    auto space = SpaceId::parse(sax.values[0]);
    auto parent = NodeId::parse(sax.values[1]);
    if (!space || !parent) return ProtocolError::InvalidArgument;
    if (!validateDirectoryName(sax.values[2])) return ProtocolError::InvalidName;
    return CreateDirectoryCommand{ space.value(), parent.value(), std::move(sax.values[2]) };
}
}  // namespace anof::http
