#pragma once
#include <asterion/futures/live_market.hpp>
#include <asterion/strategy.pb.h>
#include <map>

namespace asterion::trading {

// One instance's fresh observation window. A later provider observation seals a
// period; silence never manufactures bars or proves that a session has closed.
class StrategyBars
{
public:
    // `sequence`: the last input already delivered, when resuming from a record.
    StrategyBars(
        const strategy::proto::Instance&,
        std::int64_t started_ms,
        std::uint64_t sequence = 0);
    std::vector<strategy::proto::Bars> observe(const MarketQuote&);

private:
    struct Current
    {
        strategy::proto::Bar bar;
        std::int64_t key = 0, source_ms = 0, volume = 0;
        bool initialized = false, complete = false;
    };

    bool daily_;
    std::int64_t interval_, started_ms_;
    std::vector<std::string> contracts_;
    std::map<std::string, Current> current_;
    std::map<std::int64_t, std::map<std::string, strategy::proto::Bar>> pending_;
    std::uint64_t sequence_ = 0;
};

} // namespace asterion::trading
