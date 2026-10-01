#pragma once
#include "service_endpoint.hpp"
#include <asterion/foundation/serialization.hpp>
#include <memory>
namespace asterion::terminal {
class MarketClient {
public:
  explicit MarketClient(ServiceEndpoint endpoint);
  ~MarketClient();
  void connect(const Json& params);
  void catalog(const Json& params);
  void subscribe(const Json& instruments);
  void disconnect();
  // Current trading-day minute bars observed by the market service.
  Json minutes(const std::string& venue, const std::string& symbol);
  Json snapshot() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::terminal
