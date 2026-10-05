#pragma once
#include <asterion/foundation/serialization.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>
namespace asterion::agent {
// The durable plan of one node upgrade. The Agent drains its services, the
// candidate program resumes them, and a confirmed candidate completes the plan.
struct UpgradePlan {
  enum class Phase { draining, ready, restoring, complete };
  std::string operation;
  Phase phase = Phase::draining;
  // Each service's configuration revision when the upgrade began.
  std::map<std::string, std::string> services;
  // Processes of the previous program that must exit before draining is done.
  std::vector<std::uint64_t> processes;
  bool active() const noexcept { return phase != Phase::complete; }
};
std::string_view upgrade_phase_name(UpgradePlan::Phase phase) noexcept;
// maintenance-plan.json, version 1.
Json encode_upgrade_plan(const UpgradePlan& plan);
UpgradePlan decode_upgrade_plan(const Json& stored);
} // namespace asterion::agent
