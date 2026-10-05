#include "upgrade_plan.hpp"
#include <stdexcept>
namespace asterion::agent {
std::string_view upgrade_phase_name(UpgradePlan::Phase phase) noexcept {
  switch (phase) {
  case UpgradePlan::Phase::draining:
    return "draining";
  case UpgradePlan::Phase::ready:
    return "ready";
  case UpgradePlan::Phase::restoring:
    return "restoring";
  case UpgradePlan::Phase::complete:
    return "complete";
  }
  return "draining";
}
Json encode_upgrade_plan(const UpgradePlan& plan) {
  return {{"version", 1},
          {"operation", plan.operation},
          {"phase", upgrade_phase_name(plan.phase)},
          {"services", plan.services},
          {"processes", plan.processes}};
}
UpgradePlan decode_upgrade_plan(const Json& stored) {
  require_fields(stored, {"version", "operation", "phase", "services", "processes"});
  UpgradePlan plan;
  const auto phase = stored.at("phase").is_string() ? stored.at("phase").get<std::string>() : "";
  if (phase == "draining")
    plan.phase = UpgradePlan::Phase::draining;
  else if (phase == "ready")
    plan.phase = UpgradePlan::Phase::ready;
  else if (phase == "restoring")
    plan.phase = UpgradePlan::Phase::restoring;
  else if (phase == "complete")
    plan.phase = UpgradePlan::Phase::complete;
  else
    throw std::runtime_error("invalid upgrade plan");
  if (stored.at("version") != 1 || !stored.at("services").is_object())
    throw std::runtime_error("invalid upgrade plan");
  plan.operation = stored.at("operation").get<std::string>();
  validate_id(plan.operation);
  for (const auto& [name, revision] : stored.at("services").items()) {
    if (!revision.is_string())
      throw std::runtime_error("invalid upgrade plan");
    plan.services.emplace(name, revision.get<std::string>());
  }
  if (!stored.at("processes").is_array())
    throw std::runtime_error("invalid upgrade processes");
  for (const auto& process : stored.at("processes")) {
    if (!process.is_number_unsigned())
      throw std::runtime_error("invalid upgrade processes");
    plan.processes.push_back(process.get<std::uint64_t>());
  }
  return plan;
}
} // namespace asterion::agent
