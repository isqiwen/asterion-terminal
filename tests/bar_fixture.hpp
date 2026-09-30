#pragma once
// Test-only bar datasets. Never shipped or used as market data.
#include <asterion/foundation/id.hpp>
#include <asterion/protocol/data.hpp>
#include <string>
#include <vector>
namespace asterion::test {
inline Decimal dec(const char* value) {
  return Decimal::parse(value);
}
// A bar whose open, high, low and close are one price.
inline MarketBar flat(std::string day, std::int64_t ns, const char* price,
                      const char* volume = "10") {
  return {std::move(day), ns, dec(price), dec(price), dec(price), dec(price), dec(volume)};
}
inline MarketBar bar(std::string day, std::int64_t ns, const char* open, const char* high,
                     const char* low, const char* close, const char* volume = "10") {
  return {std::move(day), ns, dec(open), dec(high), dec(low), dec(close), dec(volume)};
}
inline protocol::v1::Contract contract(const char* venue = "SHFE", const char* symbol = "rb2610",
                                       const char* product = "rb", const char* month = "2026-10") {
  return protocol::encode_contract({{"venue", venue},
                                    {"symbol", symbol},
                                    {"currency", "CNY"},
                                    {"price_increment", "1"},
                                    {"quantity_increment", "1"},
                                    {"multiplier", "10"},
                                    {"product", product},
                                    {"delivery_month", month}});
}
// Every distinct bar day settles at settlement_price unless listed in days.
inline data::v1::BarDataset dataset(const std::vector<MarketBar>& bars,
                                    std::vector<DaySettlement> days = {},
                                    protocol::v1::Contract spec = contract(),
                                    unsigned interval_minutes = 1) {
  data::v1::BarDataset result;
  result.set_version(1);
  *result.mutable_contract() = spec;
  result.set_interval_minutes(interval_minutes);
  for (const auto& value : bars)
    *result.add_bars() = protocol::encode_bar(value);
  if (days.empty())
    for (const auto& value : bars)
      if (days.empty() || days.back().trading_day != value.trading_day)
        days.push_back({value.trading_day, value.close});
  for (const auto& day : days) {
    auto* row = result.add_days();
    row->set_trading_day(day.trading_day);
    row->mutable_settlement_price()->set_units(day.settlement_price.raw());
  }
  result.set_source("test.fixture");
  result.set_source_task_id("fixture-source");
  result.set_settlement_task_id("fixture-settlement");
  result.set_manifest_sha256(std::string(64, 'a'));
  result.set_settlement_manifest_sha256(std::string(64, 'b'));
  result.set_revision(protocol::bar_dataset_revision(result));
  protocol::validate_bar_dataset(result);
  return result;
}
inline Json dataset_json(const std::vector<MarketBar>& bars, std::vector<DaySettlement> days = {}) {
  return protocol::decode_bar_dataset(dataset(bars, std::move(days)));
}
// Rising-then-falling one-day series of count bars, one minute apart.
inline std::vector<MarketBar> series(std::size_t count, const std::string& day = "2026-09-28",
                                     std::int64_t start = 1'790'000'000'000'000'000) {
  std::vector<MarketBar> result;
  for (std::size_t i = 0; i < count; ++i) {
    const auto price = std::to_string(100 + static_cast<int>(i < count / 2 ? i : count - i) % 37);
    result.push_back(
        flat(day, start + static_cast<std::int64_t>(i) * 60'000'000'000, price.c_str()));
  }
  return result;
}
} // namespace asterion::test
