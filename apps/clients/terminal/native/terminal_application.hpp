#pragma once
#include <memory>
#include <nlohmann/json.hpp>
namespace asterion::terminal {
// Thread-safe facade. Operations touching services are serialized internally.
// While one is in progress, runtime.snapshot returns the most recent snapshot
// with "stale": true instead of queueing behind it, so status polling never
// blocks on a slow service or a long deployment.
// Historical minute queries capture a client, release the operation lock for I/O,
// and reject results when the selected research service changed in the meantime.
// Other commands are rejected with conflict while a command is active; they
// never sit in a queue and execute later against a changed workspace.
class Application {
public:
  Application();
  ~Application();
  nlohmann::json dispatch(const nlohmann::json& request);

private:
  friend struct ApplicationTestAccess;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::terminal
