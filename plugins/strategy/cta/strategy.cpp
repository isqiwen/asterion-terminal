#include "strategy.hpp"
#include "evaluation.hpp"
#include <asterion/protocol/trading.hpp>
#include <algorithm>
#include <cmath>
#include <deque>
#include <stdexcept>

namespace asterion {

Strategy::Strategy(Decimal quantity, PositionSides sides)
    : quantity_(quantity),
      sides_(sides)
{}

void Strategy::start()
{
    if (running_)
        throw std::logic_error("strategy already started");
    reset();
    last_time_ = -1;
    running_ = true;
}

void Strategy::stop() noexcept
{
    running_ = false;
}

std::optional<Decimal> Strategy::on_bar(const MarketBar& bar)
{
    if (!running_)
        throw std::logic_error("strategy is stopped");
    if (bar.timestamp_ns <= last_time_)
        throw std::invalid_argument("strategy events are out of order");
    // A rule changes its state only once it has accepted the bar.
    const auto side = decide(bar);
    last_time_ = bar.timestamp_ns;
    if (!side)
        return std::nullopt;
    if (*side > 0 && sides_ != PositionSides::short_only)
        return quantity_;
    if (*side < 0 && sides_ != PositionSides::long_only)
        return Decimal{} - quantity_;
    return Decimal{};
}

namespace {

// The mean of the latest closes over a fixed window, exact for every close a
// Decimal holds. The sum of a long window leaves the range of a price long
// before the mean does, and cross-multiplying two sums to compare their means
// leaves it sooner still, so no sum is kept: each close is split by the
// window into a quotient and a remainder, and those are summed apart.
class WindowMean
{
public:
    explicit WindowMean(std::size_t window)
        : window_(static_cast<std::int64_t>(window))
    {}

    void clear()
    {
        closes_.clear();
        quotients_ = remainders_ = 0;
    }

    bool full() const
    {
        return closes_.size() == static_cast<std::size_t>(window_);
    }

    void push(Decimal close)
    {
        if (full()) {
            quotients_ -= closes_.front().raw() / window_;
            remainders_ -= closes_.front().raw() % window_;
            closes_.pop_front();
        }
        quotients_ += close.raw() / window_;
        remainders_ += close.raw() % window_;
        closes_.push_back(close);
    }

    // This mean against another's, both windows full. Nothing is rounded.
    std::strong_ordering compare(const WindowMean& other) const
    {
        const auto [whole, part] = parts();
        const auto [other_whole, other_part] = other.parts();
        if (whole != other_whole)
            return whole <=> other_whole;
        // part / window against other_part / other.window, without dividing.
        return part * other.window_ <=> other_part * window_;
    }

private:
    // The mean as whole + part / window with 0 <= part < window. A close below
    // zero leaves a remainder below zero, which is carried into the whole.
    std::pair<std::int64_t, std::int64_t> parts() const
    {
        auto whole = quotients_ + remainders_ / window_, part = remainders_ % window_;
        if (part < 0) {
            part += window_;
            --whole;
        }
        return {whole, part};
    }

    std::int64_t window_, quotients_ = 0, remainders_ = 0;
    std::deque<Decimal> closes_;
};

class MovingAverageCross final : public Strategy
{
public:
    MovingAverageCross(Decimal quantity, PositionSides sides, std::size_t fast, std::size_t slow)
        : Strategy(quantity, sides),
          fast_(fast),
          slow_(slow)
    {}

private:
    void reset() override
    {
        fast_.clear();
        slow_.clear();
    }

    std::optional<int> decide(const MarketBar& bar) override
    {
        fast_.push(bar.close);
        slow_.push(bar.close);
        if (!slow_.full())
            return std::nullopt;
        const auto side = fast_.compare(slow_);
        return side > 0 ? 1 : side < 0 ? -1 : 0;
    }

    WindowMean fast_, slow_;
};

class ChannelBreakout final : public Strategy
{
public:
    ChannelBreakout(Decimal quantity, PositionSides sides, std::size_t entry, std::size_t exit)
        : Strategy(quantity, sides),
          entry_(entry),
          exit_(exit)
    {}

private:
    void reset() override
    {
        highs_.clear();
        lows_.clear();
        side_ = 0;
    }

    std::optional<int> decide(const MarketBar& bar) override
    {
        // The channels are those of the bars before this one.
        std::optional<int> result;
        if (highs_.size() == entry_) {
            const auto highest = [&](std::size_t count) {
                return *std::max_element(
                    highs_.end() - static_cast<std::ptrdiff_t>(count),
                    highs_.end());
            };
            const auto lowest = [&](std::size_t count) {
                return *std::min_element(
                    lows_.end() - static_cast<std::ptrdiff_t>(count),
                    lows_.end());
            };
            if (bar.close > highest(entry_))
                side_ = 1;
            else if (bar.close < lowest(entry_))
                side_ = -1;
            else if ((side_ > 0 && bar.close < lowest(exit_))
                || (side_ < 0 && bar.close > highest(exit_)))
                side_ = 0;
            result = side_;
            highs_.pop_front();
            lows_.pop_front();
        }
        highs_.push_back(bar.high);
        lows_.push_back(bar.low);
        return result;
    }

    std::size_t entry_, exit_;
    std::deque<Decimal> highs_, lows_;
    int side_ = 0;
};

class Momentum final : public Strategy
{
public:
    Momentum(Decimal quantity, PositionSides sides, std::size_t lookback)
        : Strategy(quantity, sides),
          lookback_(lookback)
    {}

private:
    void reset() override
    {
        closes_.clear();
    }

    std::optional<int> decide(const MarketBar& bar) override
    {
        std::optional<int> result;
        if (closes_.size() == lookback_) {
            result = bar.close > closes_.front() ? 1 : bar.close < closes_.front() ? -1 : 0;
            closes_.pop_front();
        }
        closes_.push_back(bar.close);
        return result;
    }

    std::size_t lookback_;
    std::deque<Decimal> closes_;
};

class BandReversion final : public Strategy
{
public:
    BandReversion(Decimal quantity, PositionSides sides, std::size_t window, Decimal width)
        : Strategy(quantity, sides),
          window_(window),
          width_(static_cast<long double>(width.raw()) / 100000000)
    {}

private:
    void reset() override
    {
        closes_.clear();
        side_ = 0;
    }

    std::optional<int> decide(const MarketBar& bar) override
    {
        closes_.push_back(bar.close);
        if (closes_.size() > window_)
            closes_.pop_front();
        if (closes_.size() < window_)
            return std::nullopt;
        // Mean and deviation are statistics of the window, in floating point and
        // relative to its first close so large prices keep their small changes.
        const auto base = closes_.front();
        long double sum = 0, squares = 0;
        for (const auto close : closes_) {
            const auto value = static_cast<long double>((close - base).raw());
            sum += value;
            squares += value * value;
        }
        const auto count = static_cast<long double>(window_);
        const auto mean = sum / count;
        const auto deviation = std::sqrt(std::max(squares / count - mean * mean, 0.0L));
        const auto distance = static_cast<long double>((bar.close - base).raw()) - mean;
        if (distance > width_ * deviation)
            side_ = -1;
        else if (distance < -width_ * deviation)
            side_ = 1;
        else if ((side_ > 0 && distance >= 0) || (side_ < 0 && distance <= 0))
            side_ = 0;
        return side_;
    }

    std::size_t window_;
    long double width_;
    std::deque<Decimal> closes_;
    int side_ = 0;
};

} // namespace

std::unique_ptr<Strategy>
make_strategy(const protocol::v1::Strategy& definition, const Instrument& instrument)
{
    instrument.validate();
    protocol::validate_strategy(definition, instrument.quantity_increment);
    const auto quantity = Decimal::from_raw(definition.quantity().units());
    const auto sides = protocol::position_sides(definition.sides());
    switch (definition.rule_case()) {
        case protocol::v1::Strategy::kMovingAverage:
            return std::make_unique<MovingAverageCross>(
                quantity,
                sides,
                definition.moving_average().fast(),
                definition.moving_average().slow());
        case protocol::v1::Strategy::kBreakout:
            return std::make_unique<ChannelBreakout>(
                quantity,
                sides,
                definition.breakout().entry(),
                definition.breakout().exit());
        case protocol::v1::Strategy::kMomentum:
            return std::make_unique<Momentum>(quantity, sides, definition.momentum().lookback());
        case protocol::v1::Strategy::kReversion:
            return std::make_unique<BandReversion>(
                quantity,
                sides,
                definition.reversion().window(),
                Decimal::from_raw(definition.reversion().width().units()));
        case protocol::v1::Strategy::kCross:
            throw std::invalid_argument("a rule over several contracts runs in backtests only");
        case protocol::v1::Strategy::RULE_NOT_SET:
            break;
    }
    throw std::invalid_argument("unknown strategy rule");
}

bool CrossSection::defines(const protocol::v1::Strategy& definition)
{
    return definition.has_cross();
}

CrossSection::CrossSection(const protocol::v1::Strategy& definition, std::size_t units)
    : factor_(definition.cross().factor()),
      reversed_(definition.cross().reversed()),
      lookback_(definition.cross().lookback()),
      volatility_(definition.cross().volatility()),
      need_(protocol::strategy_warmup(definition)),
      rebalance_(definition.cross().rebalance()),
      count_(definition.cross().count()),
      sides_(protocol::position_sides(definition.sides())),
      latest_(units),
      bars_(units)
{
    if (!defines(definition))
        throw std::invalid_argument("unknown strategy rule");
    // A unit is never wanted on both sides.
    if (units < 2 * count_)
        throw std::invalid_argument(
            "a ranking rule requires at least twice as many contracts as it holds a side");
}

void CrossSection::on_bar(std::size_t unit, std::int64_t timestamp_ns, Decimal close, Decimal term)
{
    if (timestamp_ns < time_ || (timestamp_ns == time_ && latest_.at(unit)))
        throw std::invalid_argument("strategy events are out of order");
    if (close <= Decimal{})
        throw std::invalid_argument("a ranking rule requires positive closes");
    if (timestamp_ns > time_)
        std::ranges::fill(latest_, std::nullopt);
    time_ = timestamp_ns;
    latest_.at(unit) = Bar{close, term};
}

std::optional<std::vector<double>> CrossSection::rank()
{
    const bool everywhere = std::ranges::all_of(latest_, [](const auto& bar) {
        return bar.has_value();
    });
    if (everywhere)
        for (std::size_t unit = 0; unit < bars_.size(); ++unit) {
            bars_[unit].push_back(*latest_[unit]);
            if (bars_[unit].size() > need_)
                bars_[unit].pop_front();
        }
    std::ranges::fill(latest_, std::nullopt);
    // The first ranking needs its windows behind it; later ones follow at every
    // `rebalance`-th shared bar.
    if (!everywhere || ++shared_ < need_ || (shared_ - need_) % rebalance_ != 0)
        return std::nullopt;
    // The last `count` closes of a unit, or the carry of its last bars.
    const auto last = [](const std::deque<Bar>& bars, Decimal Bar::* value, std::size_t count) {
        std::vector<Decimal> values;
        for (auto i = bars.size() - count; i < bars.size(); ++i)
            values.push_back(bars[i].*value);
        return values;
    };
    // Statistics of a factor, in binary floating point: not ledger arithmetic.
    // Each is the factor evaluation's own feature at the latest bar.
    const auto latest = [&](Feature feature,
                            const std::deque<Bar>& bars,
                            Decimal Bar::* value,
                            std::size_t window,
                            std::size_t count) {
        return *feature(last(bars, value, count), window).back();
    };
    std::vector<double> score;
    for (const auto& bars : bars_)
        switch (factor_) {
            case protocol::v1::PRICE_MOMENTUM:
                score.push_back(latest(momentum, bars, &Bar::close, lookback_, lookback_ + 1));
                break;
            case protocol::v1::TERM_STRUCTURE:
                score.push_back(latest(average, bars, &Bar::term, lookback_, lookback_));
                break;
            case protocol::v1::VOLATILITY:
                score.push_back(latest(volatility, bars, &Bar::close, lookback_, lookback_ + 1));
                break;
            default:
                throw std::invalid_argument("unknown factor");
        }
    std::vector<std::size_t> order(bars_.size());
    for (std::size_t unit = 0; unit < order.size(); ++unit)
        order[unit] = unit;
    // Those to hold long first: the highest, or the lowest of a reversed rule.
    // Equal units keep the order they were given in.
    std::ranges::stable_sort(order, [&](auto left, auto right) {
        return reversed_ ? score[left] < score[right] : score[left] > score[right];
    });
    std::vector<double> wanted(bars_.size());
    for (std::size_t i = 0; i < count_; ++i) {
        if (sides_ != PositionSides::short_only)
            wanted[order[i]] = 1;
        if (sides_ != PositionSides::long_only)
            wanted[order[order.size() - 1 - i]] = -1;
    }
    if (!volatility_)
        return wanted;
    // The held units share what they would hold alike by the inverse of their
    // volatility over the window; one without any is not held.
    std::vector<long double> inverse(bars_.size());
    long double total = 0;
    std::size_t held = 0;
    for (std::size_t unit = 0; unit < bars_.size(); ++unit) {
        if (wanted[unit] == 0)
            continue;
        if (const auto sigma =
                latest(volatility, bars_[unit], &Bar::close, volatility_, volatility_ + 1);
            sigma > 0) {
            inverse[unit] = 1 / static_cast<long double>(sigma);
            total += inverse[unit];
            ++held;
        }
    }
    for (std::size_t unit = 0; unit < bars_.size(); ++unit)
        wanted[unit] *= inverse[unit] > 0
            ? static_cast<double>(static_cast<long double>(held) * inverse[unit] / total)
            : 0;
    return wanted;
}

} // namespace asterion
