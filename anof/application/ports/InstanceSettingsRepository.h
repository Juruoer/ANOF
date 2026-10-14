#pragma once

#include <anof/domain/instance/InstanceSettings.h>
#include <optional>

namespace anof {
class SettingsReader {
public:
    virtual ~SettingsReader() = default;
    virtual Result<std::optional<InstanceSettings>> get() = 0;
};
class SettingsRepository : public SettingsReader {
public:
    virtual Result<void> insertBootstrap(const InstanceSettings&) = 0;
};
}  // namespace anof
