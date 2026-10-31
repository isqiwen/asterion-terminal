#pragma once
#include <asterion/system/progress.hpp>
#include <asterion/base/serialization.hpp>
#include <asterion/trading.pb.h>

namespace asterion::protocol {

runtime::proto::Progress encode_progress(Progress::Observation value);
Json execution_health_json(const runtime::proto::ExecutionHealth& health);
void age_execution_health(runtime::proto::ExecutionHealth& health, std::uint64_t elapsed_ms);
bool execution_health_stalled(const runtime::proto::ExecutionHealth& health);
// A managed service as its supervisor publishes it. A service reports the
// first four about itself; the supervisor observes the last two.
enum class ServiceHealth
{
    starting,
    awaiting_input,
    ready,
    degraded,
    offline,
    unresponsive
};
std::string_view service_health_name(ServiceHealth health) noexcept;
ServiceHealth trading_health_phase(const proto::Health& health);

} // namespace asterion::protocol
