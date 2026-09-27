#include "moving_average.hpp"
#include <stdexcept>
namespace asterion {
MovingAverage::MovingAverage(Instrument instrument, std::size_t fast, std::size_t slow, Decimal quantity)
    : instrument_(std::move(instrument)), fast_(fast), slow_(slow), quantity_(quantity) {
    instrument_.validate();
    if (!fast || fast >= slow || slow > 10000 || quantity <= Decimal{} || !quantity.multiple_of(instrument_.quantity_increment))
        throw std::invalid_argument("SMA requires 0 < fast < slow <= 10000 and a positive lot-aligned quantity");
}
PluginDescriptor MovingAverage::descriptor() const { return {"asterion.strategy.cta.sma-long-flat", PluginKind::strategy, plugin_contract_version, {}}; }
void MovingAverage::start() {
    if (running_) throw std::logic_error("strategy already started");
    fast_values_.clear(); slow_values_.clear(); fast_sum_ = {}; slow_sum_ = {}; last_time_ = -1; running_ = true;
}
void MovingAverage::stop() noexcept { running_ = false; }
std::optional<Decimal> MovingAverage::on_tick(const TradeTick& tick) {
    if (!running_) throw std::logic_error("strategy is stopped");
    tick.validate(instrument_);
    if (tick.timestamp_ns < last_time_) throw std::invalid_argument("strategy events are out of order");
    // Calculate checked sums before changing state; an overflow rejects the event.
    auto fast_sum = fast_sum_ + tick.price;
    auto slow_sum = slow_sum_ + tick.price;
    if (fast_values_.size() == fast_) fast_sum = fast_sum - fast_values_.front();
    if (slow_values_.size() == slow_) slow_sum = slow_sum - slow_values_.front();
    const auto fast_count = Decimal::parse(std::to_string(fast_));
    const auto slow_count = Decimal::parse(std::to_string(slow_));
    const bool ready = slow_values_.size() + 1 >= slow_;
    // Cross multiplication avoids rounding a mean before comparing signals.
    const bool bullish = ready && fast_sum * slow_count > slow_sum * fast_count;
    fast_values_.push_back(tick.price); slow_values_.push_back(tick.price);
    if (fast_values_.size() > fast_) fast_values_.pop_front();
    if (slow_values_.size() > slow_) slow_values_.pop_front();
    fast_sum_ = fast_sum; slow_sum_ = slow_sum; last_time_ = tick.timestamp_ns;
    return ready ? std::optional<Decimal>(bullish ? quantity_ : Decimal{}) : std::nullopt;
}
}
