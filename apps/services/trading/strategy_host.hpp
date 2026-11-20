#pragma once
#include <asterion/base/time.hpp>
#include <asterion/trading.pb.h>
#include <asterion/system/thread_pool.hpp>
#include <filesystem>
#include <memory>
#include "ctp_catalog.hpp"
#include "ctp_feed.hpp"

namespace asterion::trading {

class AlertWebhook;
class LiveSession;

// One account's immutable packages and one loaded strategy child. Filesystem
// and process work use their own bounded worker, never the account owner.
class StrategyHost
{
public:
    StrategyHost(
        std::filesystem::path installation,
        std::filesystem::path account,
        ctp::Configuration market,
        std::shared_ptr<AlertWebhook> alerts = nullptr,
        std::shared_ptr<const Clock> clock = default_clock());
    ~StrategyHost();
    // One of the strategy calls of trading.proto; replies with the strategies after it.
    std::future<::trading::Strategies> request(::trading::Request, LiveSession* account);
    std::future<::trading::RecordedBars> recorded_bars(::trading::GetRecordedBars);
    std::future<::trading::StrategyRun> strategy_run(::trading::GetStrategyRun);
    std::future<::trading::ContractCatalog> catalog(
        ::trading::GetCatalog,
        std::filesystem::path ctp_library,
        ctp::CatalogConfiguration configuration);
    void poll(); // I/O owner: schedules child-exit observation without waiting.
    void stop();
    bool stopped() const;

private:
    struct State;
    std::unique_ptr<State> state_;
    std::future<void> observation_, shutdown_;
    bool closing_ = false;
    std::stop_source catalog_stop_;
    ThreadPool worker_{1, 8};
    ThreadPool catalog_worker_{1, 4};
};

} // namespace asterion::trading
