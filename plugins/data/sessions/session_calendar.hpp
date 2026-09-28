#pragma once
#include <asterion/domain/trading_schedule.hpp>
#include <asterion/foundation/serialization.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Trading-session templates per product and a generator that turns a list of
// trading days into UTC sessions. This assists building settlement calendars;
// the trading days themselves always come from the caller (exchange notices),
// never from inferred holidays.
namespace asterion::sessions {
// Minutes after local midnight. A night interval whose end is not after its
// begin crosses midnight and ends on the next calendar day.
struct LocalInterval {
  int begin_minute = 0, end_minute = 0;
};
struct SessionTemplate {
  std::string venue, product;
  std::vector<LocalInterval> day;
  std::optional<LocalInterval> night;
  int utc_offset_minutes = 0;
};
class SessionCatalog {
public:
  // Validates config/futures-sessions.json (version 1).
  static SessionCatalog parse(const Json& document);
  const SessionTemplate& find(const std::string& venue, const std::string& product) const;
  // "<title>, reviewed <date>" for schedule-source evidence.
  const std::string& provenance() const noexcept { return provenance_; }

private:
  std::map<std::pair<std::string, std::string>, SessionTemplate> templates_;
  std::string provenance_;
};
struct GeneratedDay {
  std::string trading_day;
  std::vector<TradingSession> sessions;
  bool night = false;
};
// trading_days: strictly increasing YYYY-MM-DD labels. The night session of
// a trading day D runs on the evening of the previous trading day P, and only
// if every calendar day strictly between P and D is a Saturday or Sunday: the
// last working day before a statutory holiday has no night session. The first
// day needs `previous` (the trading day before it) to decide; without it that
// day gets no night session.
std::vector<GeneratedDay> generate(const SessionTemplate& session,
                                   const std::vector<std::string>& trading_days,
                                   const std::optional<std::string>& previous = std::nullopt);
} // namespace asterion::sessions
