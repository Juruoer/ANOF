#include <anof/infrastructure/config/TomlInstanceConfiguration.h>
#include <anof/application/maintenance/PrepareInstance.h>
#include <anof/infrastructure/system/internal/BoundedFileRead.h>
#include <anof/infrastructure/system/internal/InstanceFiles.h>

#include <toml++/toml.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <string_view>

namespace anof::infrastructure::config {
namespace {

template <typename T>
bool integer(const toml::node& node, T& destination) {
    const auto value = node.value_exact<std::int64_t>();
    if (!value || *value < 0 || static_cast<std::uint64_t>(*value) > std::numeric_limits<T>::max()) return false;
    destination = static_cast<T>(*value);
    return true;
}

Result<InstanceConfiguration> parseConfiguration(std::string_view text) {
    const auto table = toml::parse(text);
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
}

class TomlInstanceConfigurationLoader final : public InstanceConfigurationLoader {
public:
    Result<InstanceConfiguration> load(const InstanceLease& lease) override {
        try {
            auto directory = system::detail::instanceDirectory(lease);
            if (!directory) return directory.error();
            auto file = system::detail::openPrivateFile(directory.value(), "config.toml", false);
            if (!file) return file.error();
            auto text = system::detail::readBoundedFile(file.value().get(), 65536, ErrorCode::InvalidArgument);
            if (!text) return text.error();
            return parseConfiguration(text.value());
        } catch (const toml::parse_error&) {
            // parser 诊断可能含凭据或整行配置，禁止传播或记录 what()。
            return ErrorCode::InvalidArgument;
        } catch (const std::bad_alloc&) {
            return ErrorCode::InternalError;
        }
    }
};
}

Result<std::unique_ptr<InstanceConfigurationLoader>> makeTomlInstanceConfigurationLoader() {
    try {
        return std::unique_ptr<InstanceConfigurationLoader>(new TomlInstanceConfigurationLoader);
    } catch (const std::bad_alloc&) {
        return ErrorCode::InternalError;
    }
}

}  // namespace anof::infrastructure::config
