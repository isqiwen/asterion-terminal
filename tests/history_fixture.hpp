#pragma once
// Test-only providers and persistence fixtures. Never linked into product targets.
#include "bar_fixture.hpp"
#include "history_minutes.hpp"
#include "history_daily.hpp"
#include "task_store.hpp"
namespace asterion::test {
inline Json seed_history(const std::filesystem::path& root, const std::vector<int>& prices,
                         const std::string& id) {
  class Minutes final : public HistoricalBarPort {
  public:
    std::vector<int> prices;
    PluginDescriptor descriptor() const override {
      return {"test.minutes", PluginKind::data, plugin_contract_version, {}};
    }
    HistorySemantics semantics() const override {
      return {"tushare.ft_mins", "test.confirmed.v1", "Asia/Shanghai", "bar_end"};
    }
    void start() override {}
    void stop() noexcept override {}
    std::vector<HistoricalBar> read(const HistoricalBarRange& r, std::stop_token) override {
      std::vector<HistoricalBar> rows;
      for (std::size_t i = 0; i < prices.size(); ++i) {
        const auto price = Decimal::parse(std::to_string(prices[i]));
        rows.push_back({r.begin_ns + static_cast<std::int64_t>(i) * 60000000000LL, price, price,
                        price, price, dec("10"), dec("1000"), dec("100"), "2026-09-25"});
      }
      return rows;
    }
  } minutes;
  class Daily final : public HistoricalDailyPort {
  public:
    PluginDescriptor descriptor() const override {
      return {"test.daily", PluginKind::data, plugin_contract_version, {}};
    }
    HistorySemantics semantics() const override {
      return {"tushare.fut_daily", "test.confirmed.v1", "Asia/Shanghai", "trading_day"};
    }
    void start() override {}
    void stop() noexcept override {}
    std::vector<HistoricalDailyBar> read(const HistoricalDailyRange& r, std::stop_token) override {
      return {{r.begin,
               dec("100"),
               dec("200"),
               dec("90"),
               dec("110"),
               dec("10000"),
               dec("1000000"),
               dec("100"),
               {},
               {},
               dec("110")}};
    }
  } daily;
  if (prices.empty() || prices.size() > 20000)
    throw std::invalid_argument("invalid test prices");
  minutes.prices = prices;
  tasks::Store store(root);
  const auto begin = parse_shanghai_time("2026-09-25 09:00:00");
  const auto minute = history_files::minute_request(
      {{"version", 2},
       {"contract_id", "SHFE/rb/2026-10"},
       {"source", "tushare.ft_mins"},
       {"source_instrument", "RB2610.SHF"},
       {"interval_minutes", 1},
       {"begin_ns", std::to_string(begin)},
       {"end_ns",
        std::to_string(begin + static_cast<std::int64_t>(prices.size() - 1) * 60000000000LL)},
       {"requests_per_minute", 500}});
  const auto day = history_files::daily_request({{"version", 2},
                                                 {"contract_id", "SHFE/rb/2026-10"},
                                                 {"source", "tushare.fut_daily"},
                                                 {"source_instrument", "RB2610.SHF"},
                                                 {"begin_day", "2026-09-25"},
                                                 {"end_day", "2026-09-25"},
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
  return {{"source_task_id", id + "-bars"},
          {"settlement_task_id", id + "-settlement"},
          {"begin_day", ""},
          {"end_day", ""},
          {"price_increment", "1"},
          {"multiplier", "10"}};
}
} // namespace asterion::test
