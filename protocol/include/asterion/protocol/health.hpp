#pragma once
#include <asterion/kernel/progress.hpp>
#include <asterion/foundation/serialization.hpp>
#include <asterion/v1/trading.pb.h>
namespace asterion::protocol {
runtime::v1::Progress encode_progress(Progress::Observation value);
Json execution_health_json(const runtime::v1::ExecutionHealth& health);
void age_execution_health(runtime::v1::ExecutionHealth& health, std::uint64_t elapsed_ms);
bool execution_health_stalled(const runtime::v1::ExecutionHealth& health);
// A managed service as its supervisor publishes it. A service reports the
// first four about itself; the supervisor observes the last two.
enum class ServiceHealth { starting, awaiting_input, ready, degraded, offline, unresponsive };
std::string_view service_health_name(ServiceHealth health) noexcept;
ServiceHealth trading_health_phase(const v1::Health& health);
} // namespace asterion::protocol
