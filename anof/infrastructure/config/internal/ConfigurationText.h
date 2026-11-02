#pragma once
#include <anof/application/ports/InstancePreparation.h>

#include <string_view>

namespace anof::infrastructure::config::detail {
Result<InstanceConfiguration> parseConfiguration(std::string_view text, bool requireComplete = false);
Result<std::string> serializeConfiguration(const InstanceConfiguration&);
bool sameConfiguration(const InstanceConfiguration&, const InstanceConfiguration&);
}  // namespace anof::infrastructure::config::detail
