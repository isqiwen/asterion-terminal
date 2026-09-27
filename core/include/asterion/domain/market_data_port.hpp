#pragma once

#include <asterion/domain/market.hpp>
#include <asterion/kernel/plugin.hpp>

#include <optional>

namespace asterion {

// Pull-based historical trade stream; no claims about realtime freshness or bars.
// nullopt means confirmed end of this input; errors must throw, never look empty.
class MarketDataPort : public Plugin {
public:
    [[nodiscard]] virtual const Instrument& instrument() const noexcept = 0;
    virtual std::optional<TradeTick> next() = 0;
};
} // namespace asterion
