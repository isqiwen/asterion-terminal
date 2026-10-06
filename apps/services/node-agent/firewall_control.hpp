#pragma once
#include "agent_work.hpp"
#include <asterion/foundation/serialization.hpp>
#include <asterion/v1/node.pb.h>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
namespace asterion::agent {
// One source-scoped firewall rule of a service: what an administrator inspects
// before confirming, and, once applied, the record of the rule the Agent owns.
struct FirewallPlan {
  std::string token, service, source, peer;
  std::uint32_t port = 0;
  std::string backend, state;
  bool can_apply = false;
  std::string rule, action, verification = "not_checked";
};
// firewall/<service>.json.
Json encode_firewall_plan(const FirewallPlan& plan);
// Inspecting ("allow" or "remove") produces a plan that stays valid for five
// minutes and for the inspecting peer only; "apply" confirms exactly that plan
// after inspecting the firewall again.
class FirewallControl {
public:
  FirewallControl(std::filesystem::path root, BlockingWork blocking);
  PolledTask<void> manage(const node::v1::Firewall& operation, const std::string& peer,
                          unsigned short port, node::v1::FirewallPlan& report);

private:
  std::filesystem::path root_;
  BlockingWork blocking_;
  std::optional<FirewallPlan> plan_;
  std::chrono::steady_clock::time_point expiry_{};
};
} // namespace asterion::agent
