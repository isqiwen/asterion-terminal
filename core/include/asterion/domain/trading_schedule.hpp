#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace asterion {
struct TradingSession {
  std::int64_t begin_ns, end_ns; // Half-open UTC interval [begin, end).
};
// The caller supplies the trading-day association. This model never infers
// exchange hours, holidays or a night session's date from wall-clock time.
class TradingDaySchedule final {
public:
  TradingDaySchedule(std::string trading_day,
                     std::vector<TradingSession> sessions);
  const std::string &trading_day() const noexcept { return day_; }
  const std::vector<TradingSession> &sessions() const noexcept {
    return sessions_;
  }
  std::optional<std::size_t>
  session_index(std::int64_t timestamp_ns) const noexcept;

private:
  std::string day_;
  std::vector<TradingSession> sessions_;
};
} // namespace asterion
