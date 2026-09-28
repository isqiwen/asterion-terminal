#include <algorithm>
#include <asterion/domain/trading_schedule.hpp>
#include <charconv>
#include <chrono>
#include <iterator>
#include <utility>
#include <stdexcept>
namespace asterion {
TradingDaySchedule::TradingDaySchedule(std::string day, std::vector<TradingSession> sessions)
    : day_(std::move(day)), sessions_(std::move(sessions)) {
  if (day_.size() != 10 || day_[4] != '-' || day_[7] != '-')
    throw std::invalid_argument("expected trading day YYYY-MM-DD");
  const auto number = [&](std::size_t offset, std::size_t length) {
    unsigned value = 0;
    const auto* begin = day_.data() + offset;
    for (std::size_t i = 0; i < length; ++i)
      if (begin[i] < '0' || begin[i] > '9')
        throw std::invalid_argument("invalid trading day");
    const auto [end, error] = std::from_chars(begin, begin + length, value);
    if (error != std::errc{} || end != begin + length)
      throw std::invalid_argument("invalid trading day");
    return value;
  };
  const auto date = std::chrono::year(static_cast<int>(number(0, 4))) /
                    std::chrono::month(number(5, 2)) / std::chrono::day(number(8, 2));
  if (!date.ok() || date.year() < std::chrono::year(1970) || sessions_.empty())
    throw std::invalid_argument("invalid trading day or empty schedule");
  std::int64_t previous = 0;
  for (const auto& session : sessions_) {
    if (session.begin_ns < 0 || session.end_ns <= session.begin_ns || session.begin_ns < previous)
      throw std::invalid_argument(
          "trading sessions must be ordered, non-overlapping UTC intervals");
    previous = session.end_ns;
  }
}
std::optional<std::size_t> TradingDaySchedule::session_index(std::int64_t time) const noexcept {
  const auto it = std::upper_bound(
      sessions_.begin(), sessions_.end(), time,
      [](auto value, const TradingSession& session) { return value < session.begin_ns; });
  if (it == sessions_.begin())
    return std::nullopt;
  const auto selected = std::prev(it);
  if (time >= selected->end_ns)
    return std::nullopt;
  return static_cast<std::size_t>(selected - sessions_.begin());
}
} // namespace asterion
