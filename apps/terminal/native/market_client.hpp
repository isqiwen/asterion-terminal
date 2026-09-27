#pragma once
#include "service_endpoint.hpp"
#include <asterion/foundation/serialization.hpp>
#include <memory>
namespace asterion::terminal {
class MarketClient {
public:
  explicit MarketClient(ServiceEndpoint endpoint);
  ~MarketClient();
  void connect(const Json &params);
  void subscribe(const Json &instruments);
  void disconnect();
  Json snapshot() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::terminal
