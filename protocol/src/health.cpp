#include <asterion/protocol/health.hpp>
namespace asterion::protocol {
runtime::v1::Progress encode_progress(Progress::Observation value) {
  runtime::v1::Progress result;
  result.set_observed(value.observed);
  result.set_pending(value.pending);
  result.set_age_ms(value.age_ms);
  return result;
}
Json execution_health_json(const runtime::v1::ExecutionHealth& health) {
  const auto progress = [](const runtime::v1::Progress& p) {
    return Json{{"observed", p.observed()}, {"pending", p.pending()}, {"age_ms", p.age_ms()}};
  };
  return {{"io", progress(health.io())},
          {"state", progress(health.state())},
          {"persistence", progress(health.persistence())},
          {"initialization", progress(health.initialization())},
          {"command", progress(health.command())},
          {"business_ready", health.business_ready()}};
}
void age_execution_health(runtime::v1::ExecutionHealth& health, std::uint64_t elapsed_ms) {
  for (auto* p : {health.mutable_io(), health.mutable_state(), health.mutable_persistence(),
                  health.mutable_initialization(), health.mutable_command()})
    if (p->observed())
      p->set_age_ms(p->age_ms() + elapsed_ms);
}
bool execution_health_stalled(const runtime::v1::ExecutionHealth& e) {
  const auto stalled = [](const runtime::v1::Progress& p, bool periodic) {
    return p.observed() && p.age_ms() > 30000 && (periodic || p.pending());
  };
  return stalled(e.io(), true) || stalled(e.state(), true) || stalled(e.persistence(), false) ||
         stalled(e.initialization(), false) || stalled(e.command(), false);
}
std::string_view service_health_name(ServiceHealth health) noexcept {
  switch (health) {
  case ServiceHealth::starting:
    return "starting";
  case ServiceHealth::awaiting_input:
    return "awaiting_input";
  case ServiceHealth::ready:
    return "ready";
  case ServiceHealth::degraded:
    return "degraded";
  case ServiceHealth::offline:
    return "offline";
  case ServiceHealth::unresponsive:
    return "unresponsive";
  }
  return "degraded";
}
ServiceHealth trading_health_phase(const v1::Health& health) {
  if (!health.has_execution())
    throw std::invalid_argument("invalid service health");
  const auto& e = health.execution();
  if (health.recovery_required() || execution_health_stalled(e))
    return ServiceHealth::degraded;
  if (e.initialization().pending())
    return ServiceHealth::starting;
  if (!health.initialized())
    return ServiceHealth::awaiting_input;
  if (!e.io().observed() || !e.state().observed() || !e.persistence().observed())
    return ServiceHealth::starting;
  return e.business_ready() ? ServiceHealth::ready : ServiceHealth::awaiting_input;
}
} // namespace asterion::protocol
