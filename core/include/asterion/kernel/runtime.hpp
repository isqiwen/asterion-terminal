#pragma once
#include <asterion/kernel/plugin.hpp>
#include <asterion/kernel/configuration.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/foundation/time.hpp>
#include <asterion/kernel/observability.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/resource_registry.hpp>
#include <asterion/kernel/security.hpp>
namespace asterion {
enum class RuntimeState { created, starting, running, stopping, stopped, failed };
// Command host: lifecycle, capability-checked dispatch, configuration,
// owned resources and traces. The host serializes all calls.
class Runtime final {
public:
  explicit Runtime(std::string scope,
                   std::shared_ptr<Clock> clock = std::make_shared<SystemClock>(),
                   std::shared_ptr<Logger> logger = std::make_shared<Logger>());
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  void add_plugin(std::unique_ptr<Plugin> plugin);
  void command(std::string name, std::string capability, std::function<Json(const Json&)> handler);
  void start();
  void stop();
  Json dispatch(const std::string& principal, const std::string& method, const Json& params);
  RuntimeState state() const noexcept { return state_; }
  Configuration& configuration() noexcept { return config_; }
  AccessPolicy& access() noexcept { return access_; }
  ResourceRegistry& resources() noexcept { return resources_; }
  const Observability& observations() const noexcept { return observations_; }

private:
  struct Command {
    std::string capability;
    std::function<Json(const Json&)> handler;
  };
  std::shared_ptr<Clock> clock_;
  IdSequence ids_;
  std::shared_ptr<Logger> logger_;
  Configuration config_;
  AccessPolicy access_;
  ResourceRegistry resources_;
  Observability observations_;
  PluginManager plugins_;
  std::map<std::string, Command> commands_;
  RuntimeState state_ = RuntimeState::created;
  bool dispatching_ = false;
};
} // namespace asterion
