#pragma once
// Test-only providers and persistence fixtures. Never linked into product targets.
#include "bar_fixture.hpp"
#include "history_minutes.hpp"
#include "history_daily.hpp"
#include "data_fixture.hpp"
#include <algorithm>
#include <cctype>
namespace asterion::test {
// Minute bars spread evenly over `minute_days` (09:00 onwards, bar end, each
// with its trading day); daily rows with settlement 110 on `daily_days`.
// The contract is SHFE `product` of delivery `month`, e.g. rb2610; every daily
// row reports `open_interest`.
inline Json seed_history(
    const std::filesystem::path& root, const std::vector<int>& prices, const std::string& id,
    std::vector<std::string> minute_days = {"2026-09-25"}, std::vector<std::string> daily_days = {},
    const std::string& product = "rb", unsigned interval = 1, int settlement_price = 110,
    const std::string& normalization = "test.confirmed.v1", const std::string& month = "2026-10",
    int open_interest = 100, const std::string& data_instance = "historical-data") {
  if (daily_days.empty())
    daily_days = minute_days;
  class Minutes final : public HistoricalBarPort {
  public:
    std::vector<int> prices;
    std::string normalization;
    std::vector<std::string> days;
    HistorySemantics semantics() const override {
      return {"tushare.ft_mins", normalization, "Asia/Shanghai", "bar_end"};
    }
    std::vector<HistoricalBar> read(const HistoricalBarRange& r, std::stop_token) override {
      std::vector<HistoricalBar> rows;
      const auto per_day = (prices.size() + days.size() - 1) / days.size();
      for (std::size_t index = 0; index < days.size(); ++index) {
        const auto base = parse_shanghai_time(days[index] + " 09:00:00");
        if (base > r.end_ns ||
            base + static_cast<std::int64_t>(per_day - 1) * 60000000000LL < r.begin_ns)
          continue;
        const auto begin = index * per_day;
        const auto end = std::min(prices.size(), begin + per_day);
        for (auto i = begin; i < end; ++i) {
          const auto at = base + static_cast<std::int64_t>(i - begin) * 60000000000LL;
          if (at < r.begin_ns || at > r.end_ns)
            continue;
          const auto price = Decimal::parse(std::to_string(prices[i]));
          rows.push_back(
              {at, price, price, price, price, dec("10"), dec("1000"), dec("100"), days[index]});
        }
      }
      return rows;
    }
  } minutes;
  class Daily final : public HistoricalDailyPort {
  public:
    int settlement_price = 110, open_interest = 100;
    std::string normalization;
    HistorySemantics semantics() const override {
      return {"tushare.fut_daily", normalization, "Asia/Shanghai", "trading_day"};
    }
    std::vector<std::string> days;
    std::vector<HistoricalDailyBar> read(const HistoricalDailyRange& r, std::stop_token) override {
      std::vector<HistoricalDailyBar> rows;
      for (const auto& text : days) {
        const auto day = parse_trading_date(text);
        if (day >= r.begin && day <= r.end)
          rows.push_back({day,
                          dec("100"),
                          dec("200"),
                          dec("90"),
                          dec("110"),
                          dec("10000"),
                          dec("1000000"),
                          Decimal::parse(std::to_string(open_interest)),
                          {},
                          {},
                          Decimal::parse(std::to_string(settlement_price))});
      }
      return rows;
    }
  } daily;
  if (prices.empty() || prices.size() > 200000)
    throw std::invalid_argument("invalid test prices");
  minutes.normalization = normalization;
  daily.normalization = normalization;
  daily.settlement_price = settlement_price;
  daily.open_interest = open_interest;
  minutes.prices = prices;
  minutes.days = minute_days;
  daily.days = daily_days;
  data::Store store(root, data_instance, "task");
  const auto contract_id = "SHFE/" + product + "/" + month;
  auto upper = product;
  std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return std::toupper(c); });
  const auto source_instrument = upper + month.substr(2, 2) + month.substr(5) + ".SHF";
  const auto per_day =
      static_cast<std::int64_t>((prices.size() + minute_days.size() - 1) / minute_days.size());
  const auto begin = parse_shanghai_time(minute_days.front() + " 09:00:00");
  const auto end =
      parse_shanghai_time(minute_days.back() + " 09:00:00") + (per_day - 1) * 60000000000LL;
  const auto minute = history_files::minute_request({{"version", 2},
                                                     {"contract_id", contract_id},
                                                     {"source", "tushare.ft_mins"},
                                                     {"source_instrument", source_instrument},
                                                     {"interval_minutes", interval},
                                                     {"begin_ns", std::to_string(begin)},
                                                     {"end_ns", std::to_string(end)},
                                                     {"requests_per_minute", 500}});
  const auto day = history_files::daily_request({{"version", 2},
                                                 {"contract_id", contract_id},
                                                 {"source", "tushare.fut_daily"},
                                                 {"source_instrument", source_instrument},
                                                 {"begin_day", daily_days.front()},
                                                 {"end_day", daily_days.back()},
                                                 {"requests_per_minute", 500}});
  const auto minute_result = publish_download(store, id + "-bars", minute, minutes).minute_result();
  const auto daily_result = publish_download(store, id + "-settlement", day, daily).daily_result();
  return {{"source_dataset_ids", Json::array({minute_result.manifest_sha256()})},
          {"settlement_dataset_ids", Json::array({daily_result.manifest_sha256()})},
          {"begin_day", ""},
          {"end_day", ""},
          {"price_increment", "1"},
          {"multiplier", "10"}};
}
} // namespace asterion::test
