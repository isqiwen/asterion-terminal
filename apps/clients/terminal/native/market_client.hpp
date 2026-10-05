#pragma once
#include "service_endpoint.hpp"
#include "service_io.hpp"
#include "market_snapshot.hpp"
#include <asterion/foundation/serialization.hpp>
#include <memory>
namespace asterion::terminal {
class Application;
class MarketClient {
public:
  [[nodiscard]] static std::future<std::shared_ptr<MarketClient>> open(ServiceIo&, ServiceEndpoint);
  ~MarketClient();
  MarketClient(const MarketClient&) = delete;
  MarketClient& operator=(const MarketClient&) = delete;
  [[nodiscard]] std::future<void> connect(const Json& params);
  [[nodiscard]] std::future<void> catalog(const Json& params);
  [[nodiscard]] std::future<void> subscribe(const Json& instruments);
  [[nodiscard]] std::future<void> disconnect();
  // Current trading-day minute bars observed by the market service.
  [[nodiscard]] std::future<Json> minutes(const std::string& venue, const std::string& symbol);
  [[nodiscard]] std::future<Json> snapshot() const;

private:
  friend class Application;
  // Capture selected service data in one I/O owner turn.
  MarketProjection owner_read() const;
  MarketClient(ServiceIo&, ServiceEndpoint);
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
} // namespace asterion::terminal
