#pragma once
#include <asterion/futures/live_market.hpp>
#include <asterion/strategy.pb.h>
#include <map>

namespace asterion::trading {

// One instance's fresh observation window. A period is delivered once every
// contract has an observation after it, or once any contract's observation is
// past its end by the freshness limit, when no quote of it can still arrive.
// A contract silent in a period, closed or halted, is absent from it: silence
// never manufactures bars, and a quote of a period already delivered goes to
// that contract's next bar.
class StrategyBars
{
public:
    // `sequence`: the last input already delivered, when resuming from a record.
    StrategyBars(const ::strategy::Instance&, std::int64_t started_ms, std::uint64_t sequence = 0);
    std::vector<::strategy::Bars> observe(const MarketQuote&);

private:
    std::vector<::strategy::Bars> deliver();

    struct Current
    {
        ::strategy::Bar bar;
        Decimal high, low;
        std::int64_t key = 0, source_ms = 0, volume = 0;
        bool initialized = false, complete = false;
    };

    bool daily_;
    std::int64_t interval_, started_ms_;
    std::vector<std::string> contracts_;
    std::map<std::string, Current> current_;
    std::map<std::int64_t, std::map<std::string, ::strategy::Bar>> pending_;
    // The latest observation of any contract, and the last period delivered.
    std::int64_t now_ = 0, delivered_ = -1;
    std::uint64_t sequence_ = 0;
};

} // namespace asterion::trading
