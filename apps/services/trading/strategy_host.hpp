#pragma once
#include <asterion/strategy.pb.h>
#include <asterion/system/thread_pool.hpp>
#include <filesystem>
#include <memory>
#include "ctp_feed.hpp"

namespace asterion::trading {

class LiveSession;

// One account's immutable packages and one loaded strategy child. Filesystem
// and process work use their own bounded worker, never the account owner.
class StrategyHost
{
public:
    StrategyHost(
        std::filesystem::path installation,
        std::filesystem::path account,
        ctp::Configuration market);
    ~StrategyHost();
    std::future<strategy::proto::State> request(strategy::proto::Request, LiveSession* account);
    void poll(); // I/O owner: schedules child-exit observation without waiting.
    void stop();
    bool stopped() const;

private:
    struct State;
    std::unique_ptr<State> state_;
    std::future<void> observation_, shutdown_;
    bool closing_ = false;
    ThreadPool worker_{1, 8};
};

} // namespace asterion::trading
