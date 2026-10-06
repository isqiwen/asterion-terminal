#include "firewall_control.hpp"
#include "firewall.hpp"
#include "managed_paths.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <fstream>
#include <stdexcept>
namespace asterion::agent {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace {
// The rule a previous confirmation left in place.
struct OwnedRule {
  std::string source, rule;
};
// What the firewall reports about itself right now.
struct Observation {
  std::string backend, state;
};
Observation inspect(const std::string& os, const std::string& peer) {
  const auto observed = node::run_firewall_script(node::firewall_inspection(os, peer));
  return {observed.at("backend").get<std::string>(), observed.at("state").get<std::string>()};
}
} // namespace
Json encode_firewall_plan(const FirewallPlan& plan) {
  return {{"token", plan.token},
          {"service", plan.service},
          {"source", plan.source},
          {"peer", plan.peer},
          {"port", plan.port},
          {"backend", plan.backend},
          {"state", plan.state},
          {"can_apply", plan.can_apply},
          {"rule", plan.rule},
          {"action", plan.action},
          {"verification", plan.verification}};
}
FirewallControl::FirewallControl(fs::path root, BlockingWork blocking)
    : root_(std::move(root)), blocking_(std::move(blocking)) {}
PolledTask<void> FirewallControl::manage(const node::v1::Firewall& operation,
                                         const std::string& peer, unsigned short port,
                                         node::v1::FirewallPlan& report) {
  const auto os = current_platform().os;
  const auto file = root_ / "firewall" / (operation.service_id() + ".json");
  std::optional<OwnedRule> owned;
  co_await blocking_([&] {
    require_managed_path(file.parent_path());
    require_managed_path(file);
    if (fs::exists(file)) {
      if (fs::file_size(file) > 65536)
        throw std::invalid_argument("invalid firewall record");
      std::ifstream input(file);
      const auto stored = Json::parse(input);
      owned =
          OwnedRule{stored.at("source").get<std::string>(), stored.at("rule").get<std::string>()};
    }
  });
  if (operation.action() == "allow" || operation.action() == "remove") {
    if (!operation.token().empty())
      throw std::invalid_argument("inspection does not accept a confirmation token");
    plan_.reset();
    Observation observed;
    co_await blocking_([&] { observed = inspect(os, peer); });
    const bool remove = operation.action() == "remove";
    if (!remove && owned && owned->source != peer)
      throw std::invalid_argument("remove previous source rule before changing source");
    FirewallPlan plan;
    plan.token = unique_process_id();
    plan.service = operation.service_id();
    plan.source = remove && owned ? owned->source : peer;
    plan.peer = peer;
    plan.port = port;
    plan.backend = observed.backend;
    plan.state = observed.state;
    plan.can_apply = observed.state == "active" && observed.backend == "ufw" && (!remove || owned);
    plan.rule = owned ? owned->rule : "asterion-" + unique_process_id();
    plan.action = operation.action();
    plan_ = std::move(plan);
    expiry_ = std::chrono::steady_clock::now() + 5min;
  } else if (operation.action() == "apply") {
    if (!plan_ || plan_->token != operation.token() || plan_->service != operation.service_id() ||
        plan_->peer != peer || std::chrono::steady_clock::now() > expiry_)
      throw std::invalid_argument("firewall confirmation expired; inspect again");
    auto plan = std::move(*plan_);
    plan_.reset();
    Observation observed;
    co_await blocking_([&] { observed = inspect(os, peer); });
    if (!plan.can_apply || observed.state != "active" || observed.backend != plan.backend)
      throw std::invalid_argument("firewall state changed; inspect again");
    const bool remove = plan.action == "remove";
    if (remove && (!owned || owned->rule != plan.rule || owned->source != plan.source))
      throw std::invalid_argument("no owned firewall rule");
    co_await blocking_([&] {
      if (!remove) {
        fs::create_directory(file.parent_path());
        replace_file_durably(file, encode_firewall_plan(plan).dump());
      }
      const auto changed = node::run_firewall_script(
          node::firewall_change(os, plan.source, port, plan.rule, remove));
      if (changed != Json{{"changed", true}})
        throw std::runtime_error("invalid firewall result");
      if (remove)
        fs::remove(file);
    });
    plan.can_apply = false;
    plan.state = remove ? "removed" : "applied";
    plan_ = std::move(plan);
  } else
    throw std::invalid_argument("unsupported firewall action");
  report.set_token(plan_->token);
  report.set_source(plan_->source);
  report.set_port(plan_->port);
  report.set_backend(plan_->backend);
  report.set_state(plan_->state);
  report.set_can_apply(plan_->can_apply);
  report.set_rule(plan_->rule);
  report.set_action(plan_->action);
  report.set_verification(plan_->verification);
}
} // namespace asterion::agent
