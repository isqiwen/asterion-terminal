#pragma once
#include <memory>
#include <nlohmann/json.hpp>
namespace asterion::terminal {
// Thread-safe facade. Operations touching services are serialized internally.
// While one is in progress, runtime.snapshot returns the most recent snapshot
// with "stale": true instead of queueing behind it, so status polling never
// blocks on a slow service or a long deployment.
class Application {
public:
  Application();
  ~Application();
  nlohmann::json dispatch(const nlohmann::json& request);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::terminal
