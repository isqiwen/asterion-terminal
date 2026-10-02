#pragma once
#include <asterion/kernel/plugin.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/foundation/time.hpp>
#include <asterion/kernel/observability.hpp>
#include <asterion/kernel/logger.hpp>
namespace asterion {
enum class RuntimeState { created, starting, running, stopping, stopped, failed };
// Command host: lifecycle, named command dispatch and traces. Every command
// is trusted in-process code; callers serialize as they need.
class Runtime final {
public:
  explicit Runtime(std::string scope,
                   std::shared_ptr<Clock> clock = std::make_shared<SystemClock>(),
                   std::shared_ptr<Logger> logger = std::make_shared<Logger>());
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  void add_plugin(std::unique_ptr<Plugin> plugin);
  void command(std::string name, std::function<Json(const Json&)> handler);
  void start();
  void stop();
  Json dispatch(const std::string& method, const Json& params);
  RuntimeState state() const noexcept { return state_; }
  const Observability& observations() const noexcept { return observations_; }

private:
  using Command = std::function<Json(const Json&)>;
  std::shared_ptr<Clock> clock_;
  IdSequence ids_;
  std::shared_ptr<Logger> logger_;
  Observability observations_;
  PluginManager plugins_;
  std::map<std::string, Command> commands_;
  RuntimeState state_ = RuntimeState::created;
};
} // namespace asterion
