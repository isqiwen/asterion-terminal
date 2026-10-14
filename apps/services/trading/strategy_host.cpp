#include "strategy_host.hpp"
#include "strategy.hpp"
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/v1/market.pb.h>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
namespace asterion::trading {
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr auto poll_interval = 250ms;
constexpr auto reply_timeout = 2s;
// Reading bars is repeatable; a market service silent for this long ends the run.
constexpr auto silence_limit = 10s;
struct Stopped {};
market::v1::MinuteSeries read(ipc::RpcClient& client, const StrategyHost::Definition& definition,
                              std::uint64_t sequence, const std::stop_token& stop) {
  market::v1::Request request;
  request.set_version(1);
  request.set_service_id(definition.market_service);
  request.set_correlation_id("strategy." + std::to_string(sequence));
  auto* instrument = request.mutable_minutes()->mutable_instrument();
  instrument->set_venue(definition.instrument.id.venue);
  instrument->set_symbol(definition.instrument.id.symbol);
  auto pending = client.request(request.SerializeAsString(), reply_timeout);
  while (pending.wait_for(0s) != std::future_status::ready) {
    if (stop.stop_requested())
      throw Stopped{};
    client.poll(50ms);
  }
  const auto payload = pending.get();
  market::v1::Response response;
  if (!response.ParseFromString(*payload) || response.version() != 1 ||
      response.service_id() != definition.market_service ||
      response.correlation_id() != request.correlation_id() || !response.has_minutes())
    throw std::runtime_error("market service reply is invalid");
  return response.minutes();
}
void run(const std::stop_token& stop, const StrategyHost::Definition& definition,
         Strategy& strategy, const StrategyHost::Report& report) {
  ipc::RpcClient client(definition.market_endpoint, 1, PayloadBudget{1 << 20}, 1 << 20);
  // The trading day being read and where its observation began.
  std::string day;
  std::int64_t fed = 0, first = 0;
  auto heard = Clock::now();
  for (std::uint64_t sequence = 1; !stop.stop_requested(); ++sequence) {
    std::optional<market::v1::MinuteSeries> series;
    try {
      series = read(client, definition, sequence, stop);
    } catch (const Stopped&) {
      return;
    } catch (const std::exception&) {
      if (Clock::now() - heard > silence_limit)
        throw std::runtime_error("market service did not answer; the strategy stopped");
    }
    if (series) {
      heard = Clock::now();
      if (series->trading_day().size() != 8)
        throw std::runtime_error(
            "market service has no observations of this contract; subscribe to it first");
      // A new trading day is a new series. Within one day a market service
      // begins observing once in its life: a series that began again is
      // another process's, and what the first one observed after the last bar
      // fed is lost whether or not this reply brings a completed bar.
      if (series->trading_day() == day && series->first_observation_ms() != first)
        throw std::runtime_error("the market service restarted; the strategy stopped");
      day = series->trading_day();
      first = series->first_observation_ms();
      const auto trading_day = day.substr(0, 4) + "-" + day.substr(4, 2) + "-" + day.substr(6, 2);
      // The last bar is still forming, and the minute in which observation
      // began is partial: neither is a completed bar. Every completed bar of
      // the read feeds the strategy before anything is reported.
      std::optional<StrategyHost::Bar> latest;
      for (int index = 0; index + 1 < series->bars_size(); ++index) {
        const auto& source = series->bars(index);
        if (source.start_ms() <= fed || source.start_ms() <= first)
          continue;
        // Bars that follow lost observations are not the market's bars. An
        // interruption after the last bar of a day, such as the feed closing
        // overnight, loses nothing the strategy would have seen.
        if (series->interrupted())
          throw std::runtime_error("market observations were interrupted; the strategy stopped");
        const MarketBar bar{trading_day,
                            source.start_ms() * 1'000'000,
                            Decimal::parse(source.open()),
                            Decimal::parse(source.high()),
                            Decimal::parse(source.low()),
                            Decimal::parse(source.close()),
                            Decimal::parse(std::to_string(source.volume()))};
        // What the market service observed is a bar of the contract's units.
        bar.validate(definition.instrument);
        const auto target = strategy.on_bar(bar);
        fed = source.start_ms();
        latest = StrategyHost::Bar{day, fed, bar.close, target, latest ? latest->count + 1 : 1};
      }
      if (latest)
        report(std::move(*latest));
    }
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock lock(mutex);
    wake.wait_for(lock, stop, poll_interval, [] { return false; });
  }
}
} // namespace
StrategyHost::StrategyHost(Definition definition, Report report) {
  auto strategy = make_strategy(definition.strategy, definition.instrument);
  strategy->start();
  thread_ = std::jthread([definition = std::move(definition), report = std::move(report),
                          strategy = std::move(strategy)](std::stop_token stop) mutable {
    try {
      run(stop, definition, *strategy, report);
    } catch (const std::exception& error) {
      if (!stop.stop_requested())
        report(Failure{error.what()});
    }
  });
}
} // namespace asterion::trading
