#pragma once
// Test-only providers and persistence fixtures. Never linked into product targets.
#include "bar_fixture.hpp"
#include "history_minutes.hpp"
#include "history_daily.hpp"
#include "task_store.hpp"
#include <algorithm>
#include <cctype>
namespace asterion::test {
// Minute bars spread evenly over `minute_days` (09:00 onwards, bar end, each
// with its trading day); daily rows with settlement 110 on `daily_days`.
// The contract is SHFE `product` 2026-10, e.g. rb2610.
inline Json seed_history(const std::filesystem::path& root, const std::vector<int>& prices,
                         const std::string& id,
                         std::vector<std::string> minute_days = {"2026-09-25"},
                         std::vector<std::string> daily_days = {},
                         const std::string& product = "rb", unsigned interval = 1,
                         int settlement_price = 110,
                         const std::string& normalization = "test.confirmed.v1") {
  if (daily_days.empty())
    daily_days = minute_days;
  class Minutes final : public HistoricalBarPort {
  public:
    std::vector<int> prices;
    std::string normalization;
    std::vector<std::string> days;
    PluginDescriptor descriptor() const override {
      return {"test.minutes", PluginKind::data, plugin_contract_version, {}};
    }
    HistorySemantics semantics() const override {
      return {"tushare.ft_mins", normalization, "Asia/Shanghai", "bar_end"};
    }
    void start() override {}
    void stop() noexcept override {}
    std::vector<HistoricalBar> read(const HistoricalBarRange& r, std::stop_token) override {
      std::vector<HistoricalBar> rows;
      const auto per_day = (prices.size() + days.size() - 1) / days.size();
      for (std::size_t i = 0; i < prices.size(); ++i) {
        const auto& day = days[i / per_day];
        const auto at = parse_shanghai_time(day + " 09:00:00") +
                        static_cast<std::int64_t>(i % per_day) * 60000000000LL;
        if (at < r.begin_ns || at > r.end_ns)
          continue;
        const auto price = Decimal::parse(std::to_string(prices[i]));
        rows.push_back({at, price, price, price, price, dec("10"), dec("1000"), dec("100"), day});
      }
      return rows;
    }
  } minutes;
  class Daily final : public HistoricalDailyPort {
  public:
    int settlement_price = 110;
    std::string normalization;
    PluginDescriptor descriptor() const override {
      return {"test.daily", PluginKind::data, plugin_contract_version, {}};
    }
    HistorySemantics semantics() const override {
      return {"tushare.fut_daily", normalization, "Asia/Shanghai", "trading_day"};
    }
    void start() override {}
    void stop() noexcept override {}
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
                          dec("100"),
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
  minutes.prices = prices;
  minutes.days = minute_days;
  daily.days = daily_days;
  tasks::Store store(root);
  const auto contract_id = "SHFE/" + product + "/2026-10";
  auto upper = product;
  std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return std::toupper(c); });
  const auto source_instrument = upper + "2610.SHF";
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
  auto finish = [&](const std::string& task, auto input, auto& provider) {
    store.submit(task, input, "explicit-test-fixture");
    research::v1::TaskAttempt attempt;
    attempt.set_token(store.claim(task));
    *attempt.mutable_task() = store.get(task);
    store.download_attempt(attempt);
    research::v1::TaskFinish completed;
    completed.set_id(task);
    completed.set_token(attempt.token());
    if constexpr (std::is_same_v<decltype(input), data::v1::MinuteDownload>) {
      history_files::download_minutes(provider, history_files::minute_range(input),
                                      attempt.output_directory(), 500);
      *completed.mutable_minutes() = history_files::minute_result(attempt.output_directory());
    } else {
      history_files::download_daily(provider, history_files::daily_range(input),
                                    attempt.output_directory(), 500);
      *completed.mutable_daily() = history_files::daily_result(attempt.output_directory());
    }
    auto verified = store.prepare_finish(completed);
    verified.verify();
    store.finish(std::move(verified));
  };
  finish(id + "-bars", minute, minutes);
  finish(id + "-settlement", day, daily);
  return {
      {"source_dataset_ids", Json::array({store.minute_result(id + "-bars").manifest_sha256()})},
      {"settlement_dataset_ids",
       Json::array({store.daily_result(id + "-settlement").manifest_sha256()})},
      {"begin_day", ""},
      {"end_day", ""},
      {"price_increment", "1"},
      {"multiplier", "10"}};
}
} // namespace asterion::test
