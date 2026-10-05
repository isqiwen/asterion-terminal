#pragma once
#include <asterion/domain/history_identity.hpp>
#include <stop_token>
#include <vector>
namespace asterion {
// Timestamp is the source's bar label; this type does not invent a trading day
// or an intrabar execution path. Consumers must honor the dataset's convention.
struct HistoricalBar {
  std::int64_t timestamp_ns = 0;
  Decimal open, high, low, close, volume, amount, open_interest;
  std::string trading_day = {}; // Empty when the provider cannot establish it.
  auto operator<=>(const HistoricalBar&) const = default;
  void validate() const;
};
struct HistoricalBarRange {
  HistoryIdentity instrument;
  unsigned interval_minutes = 1;
  std::int64_t begin_ns = 0, end_ns = 0; // inclusive source labels
  std::string source;
  std::string source_instrument = {};
};
class HistoricalBarPort {
public:
  virtual ~HistoricalBarPort() = default;
  virtual HistorySemantics semantics() const = 0;
  virtual std::vector<HistoricalBar> read(const HistoricalBarRange&, std::stop_token) = 0;
};
} // namespace asterion
