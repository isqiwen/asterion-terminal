#pragma once
#include "service_endpoint.hpp"
#include <asterion/protocol/strategy.hpp>
namespace asterion::terminal {
class StrategyClient {
public:
  explicit StrategyClient(ServiceEndpoint endpoint);
  void create(const strategy::v1::Config &config);
  Json status();
  const strategy::v1::Config &config() const;

private:
  ServiceEndpoint endpoint_;
  strategy::v1::Config config_;
  Json last_ = nullptr;
  strategy::v1::Response call(strategy::v1::Request request,
                              bool wait_for_start = false);
  void observe(const strategy::v1::Response &response);
};
} // namespace asterion::terminal
