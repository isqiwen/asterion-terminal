#pragma once
#include <asterion/domain/market.hpp>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <variant>
namespace asterion::trading {
// Runs one moving-average strategy on the completed minute bars of one
// contract, read from the market service on this machine, across trading
// days. It owns the data source and the strategy and has no access to the
// account: it reports each bar with the target the strategy asked for, or the
// one failure that ends it. It never feeds a bar it did not observe whole.
class StrategyHost {
public:
  struct Definition {
    std::string market_endpoint, market_service;
    Instrument instrument;
    std::size_t fast = 0, slow = 0;
    Decimal quantity;
  };
  struct Bar {
    // As the market service states it (YYYYMMDD).
    std::string trading_day;
    std::int64_t start_ms = 0;
    Decimal close;
    std::optional<Decimal> target;
  };
  struct Failure {
    std::string reason;
  };
  // Called on the host thread, in bar order; nothing follows a failure.
  using Report = std::function<void(std::variant<Bar, Failure>)>;
  // Throws for parameters the strategy rejects. Destruction stops the thread.
  StrategyHost(Definition definition, Report report);

private:
  std::jthread thread_;
};
} // namespace asterion::trading
