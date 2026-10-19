#include <anof/infrastructure/config/internal/ConfigurationText.h>
#include <anof/application/maintenance/PrepareInstance.h>
#include <toml++/toml.hpp>
#include <cmath>
#include <limits>
#include <sstream>
#include <iomanip>
#include <locale>
#include <new>

namespace anof::infrastructure::config::detail {
namespace {
template <typename T>
bool integer(const toml::node& node, T& destination) {
    const auto value = node.value_exact<std::int64_t>();
    if (!value || *value < 0 || static_cast<std::uint64_t>(*value) > std::numeric_limits<T>::max()) return false;
    destination = static_cast<T>(*value);
    return true;
}

}  // namespace
Result<InstanceConfiguration> parseConfiguration(std::string_view text, bool requireComplete) {
    try {
        const auto table = toml::parse(text);
        if (requireComplete && table.size() != 8) return ErrorCode::InvalidArgument;
        InstanceConfiguration configuration;
        for (const auto& [key, node] : table) {
            const auto name = key.str();
            if (name == "listen_address") {
                const auto value = node.value_exact<std::string>();
                if (!value) return ErrorCode::InvalidArgument;
                configuration.listenAddress = *value;
            } else if (name == "development_http") {
                const auto value = node.value_exact<bool>();
                if (!value) return ErrorCode::InvalidArgument;
                configuration.developmentHttp = *value;
            } else if (name == "port") {
                if (!integer(node, configuration.port)) return ErrorCode::InvalidArgument;
            } else if (name == "metadata_queue") {
                if (!integer(node, configuration.metadataQueue)) return ErrorCode::InvalidArgument;
            } else if (name == "http_in_flight") {
                if (!integer(node, configuration.httpInFlight)) return ErrorCode::InvalidArgument;
            } else if (name == "disk_safety_min_bytes") {
                const auto value = node.value_exact<std::int64_t>();
                if (!value) return ErrorCode::InvalidArgument;
                auto parsed = ByteCount::from(*value);
                if (!parsed) return parsed.error();
                configuration.diskSafety.minimum = parsed.value();
            } else if (name == "disk_safety_percent") {
                const auto value = node.value_exact<std::int64_t>();
                if (!value) return ErrorCode::InvalidArgument;
                auto parsed = SafetyPercent::from(*value);
                if (!parsed) return parsed.error();
                configuration.diskSafety.percent = parsed.value();
            } else if (name == "shutdown_grace_seconds") {
                if (const auto value = node.value_exact<double>()) configuration.shutdownGraceSeconds = *value;
                else if (const auto whole = node.value_exact<std::int64_t>()) configuration.shutdownGraceSeconds = static_cast<double>(*whole);
                else return ErrorCode::InvalidArgument;
                if (!std::isfinite(configuration.shutdownGraceSeconds)) return ErrorCode::InvalidArgument;
            } else {
                return ErrorCode::InvalidArgument;
            }
        }
        auto valid = validateInstanceConfiguration(configuration);
        if (!valid) return valid.error();
        return configuration;
    } catch (const toml::parse_error&) { return ErrorCode::InvalidArgument; }
    catch (const std::bad_alloc&) { return ErrorCode::InternalError; }
}


Result<std::string> serializeConfiguration(const InstanceConfiguration& configuration) {
    auto valid = validateInstanceConfiguration(configuration);
    if (!valid) return valid.error();
    try {
        std::ostringstream text;
        text.imbue(std::locale::classic());
        text << "listen_address = \"127.0.0.1\"\n";
        text << "port = " << configuration.port << '\n';
        text << "development_http = true\n";
        text << "metadata_queue = " << configuration.metadataQueue << '\n';
        text << "http_in_flight = " << configuration.httpInFlight << '\n';
        text << "shutdown_grace_seconds = " << std::setprecision(std::numeric_limits<double>::max_digits10) << configuration.shutdownGraceSeconds << '\n';
        text << "disk_safety_min_bytes = " << configuration.diskSafety.minimum.value() << '\n';
        text << "disk_safety_percent = " << configuration.diskSafety.percent.value() << '\n';
        return text.str();
    } catch (const std::bad_alloc&) { return ErrorCode::InternalError; }
}
bool sameConfiguration(const InstanceConfiguration& a, const InstanceConfiguration& b) {
    return a.listenAddress == b.listenAddress && a.port == b.port && a.developmentHttp == b.developmentHttp &&
           a.metadataQueue == b.metadataQueue && a.httpInFlight == b.httpInFlight && a.shutdownGraceSeconds == b.shutdownGraceSeconds &&
           a.diskSafety.minimum == b.diskSafety.minimum && a.diskSafety.percent == b.diskSafety.percent;
}
}  // namespace anof::infrastructure::config::detail
