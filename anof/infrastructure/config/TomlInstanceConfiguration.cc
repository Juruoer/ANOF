#include <anof/infrastructure/config/TomlInstanceConfiguration.h>
#include <anof/infrastructure/config/internal/ConfigurationText.h>
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
            return detail::parseConfiguration(text.value());
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
