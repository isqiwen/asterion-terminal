#pragma once
#include <asterion/domain/market.hpp>
#include <asterion/v1/trading.pb.h>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <variant>

namespace asterion::trading {

// Runs one strategy on the completed minute bars of one
// contract, read from the market service on this machine, across trading
// days. It owns the data source and the strategy and has no access to the
// account: after a read that brought completed bars it reports the latest of
// them with the target the strategy holds after it, or the one failure that
// ends it. It never feeds a bar it did not observe whole.
class StrategyHost
{
public:
    struct Definition
    {
        std::string market_endpoint, market_service;
        Instrument instrument;
        protocol::v1::Strategy strategy;
    };

    struct Bar
    {
        // As the market service states it (YYYYMMDD).
        std::string trading_day;
        std::int64_t start_ms = 0;
        Decimal close;
        // The position wanted in lots: positive long, negative short.
        std::optional<Decimal> target;
        // Completed bars the read fed the strategy, this one last. What it wanted
        // after the earlier ones is never reported: those bars were already past
        // when they were read, as all but the last are when a run starts.
        std::uint64_t count = 1;
    };

    struct Failure
    {
        std::string reason;
    };

    // Called on the host thread, once a read, in bar order; nothing follows a failure.
    using Report = std::function<void(std::variant<Bar, Failure>)>;
    // Throws for parameters the strategy rejects. Destruction stops the thread.
    StrategyHost(Definition definition, Report report);

private:
    std::jthread thread_;
};

} // namespace asterion::trading
