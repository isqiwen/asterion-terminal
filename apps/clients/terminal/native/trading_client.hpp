#pragma once
#include <asterion/foundation/serialization.hpp>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include "service_endpoint.hpp"
#include <filesystem>
namespace asterion::terminal {
// Terminal owns the process connection, never the authoritative trading ledger.
class NodeClient;
enum class TradingMode { paper, live };
class TradingClient {
public:
  TradingClient(const std::filesystem::path& directory, TradingMode mode,
                const Json& manifest = nullptr);
  TradingClient(const ServiceEndpoint& remote, TradingMode mode);
  ~TradingClient();
  void create(const Json& manifest);
  // Live sessions only. Credentials pass through to the service and are not kept.
  void connect_broker(std::string password, std::string auth_code);
  void disconnect_broker();
  void query_costs();
  void reconnect();
  Json connection() const;
  ServiceEndpoint endpoint() const;
  void execute(const Json& command);
  Json snapshot();
  std::uint64_t process_id() const;

private:
  std::unique_ptr<NodeClient> node_;
  void monitor();
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::jthread heartbeat_;
  unsigned int restarts_ = 0, reconnects_ = 0, reconnect_attempts_ = 0;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::terminal
