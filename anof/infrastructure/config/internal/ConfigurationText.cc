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
        if (text.size() > 65536) return ErrorCode::InvalidArgument;
        const auto table = toml::parse(text);
        if (requireComplete && table.size() != 13) return ErrorCode::InvalidArgument;
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
            } else if (name == "web_enabled") {
                const auto value = node.value_exact<bool>();
                if (!value) return ErrorCode::InvalidArgument;
                configuration.webEnabled = *value;
            } else if (name == "public_origin") {
                const auto value = node.value_exact<std::string>();
                if (!value) return ErrorCode::InvalidArgument;
                configuration.publicOrigin = *value;
            } else if (name == "trusted_proxies") {
                const auto* values = node.as_array();
                if (!values || values->size() > 2) return ErrorCode::InvalidArgument;
                for (const auto& item : *values) {
                    const auto value = item.value_exact<std::string>();
                    if (!value) return ErrorCode::InvalidArgument;
                    configuration.trustedProxies.push_back(*value);
                }
            } else if (name == "audit_retention_days") {
                const auto value = node.value_exact<std::int64_t>();
                if (!value) return ErrorCode::InvalidArgument;
                auto parsed = AuditRetentionDays::from(*value);
                if (!parsed) return parsed.error();
                configuration.auditRetentionDays = parsed.value();
            } else if (name == "audit_batch_size") {
                const auto value = node.value_exact<std::int64_t>();
                if (!value) return ErrorCode::InvalidArgument;
                auto parsed = AuditBatchSize::from(*value);
                if (!parsed) return parsed.error();
                configuration.auditBatchSize = parsed.value();
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
        text << "listen_address = " << std::quoted(configuration.listenAddress) << '\n';
        text << "port = " << configuration.port << '\n';
        text << "development_http = " << (configuration.developmentHttp ? "true" : "false") << '\n';
        text << "web_enabled = " << (configuration.webEnabled ? "true" : "false") << '\n';
        text << "public_origin = " << std::quoted(configuration.publicOrigin) << '\n';
        text << "trusted_proxies = [";
        for (std::size_t i = 0; i < configuration.trustedProxies.size(); ++i) {
            if (i) text << ", ";
            text << std::quoted(configuration.trustedProxies[i]);
        }
        text << "]\n";
        text << "audit_retention_days = " << configuration.auditRetentionDays.value() << '\n';
        text << "audit_batch_size = " << configuration.auditBatchSize.value() << '\n';
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
           a.diskSafety.minimum == b.diskSafety.minimum && a.diskSafety.percent == b.diskSafety.percent &&
           a.webEnabled == b.webEnabled && a.publicOrigin == b.publicOrigin && a.trustedProxies == b.trustedProxies &&
           a.auditRetentionDays == b.auditRetentionDays && a.auditBatchSize == b.auditBatchSize;
}
}  // namespace anof::infrastructure::config::detail
