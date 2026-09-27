#pragma once

#include <asterion/domain/market.hpp>

#include <set>
#include <string>

namespace asterion {

enum class Side { buy, sell };
enum class OrderState { pending, accepted, partially_filled, filled, cancelled, rejected };

struct LimitOrder {
    std::string id;
    InstrumentId instrument;
    Side side;
    Decimal quantity;
    Decimal limit_price;
    void validate(const Instrument& spec) const;
};

struct Fill {
    std::string execution_id;
    std::string order_id;
    Decimal quantity;
    Decimal price;
    auto operator<=>(const Fill&) const = default;
};

// Local normalized order state. A connector must normalize exchange-specific
// cancel/replace and out-of-order reports before using this state machine.
class Order final {
public:
    Order(LimitOrder request, Instrument spec);
    void accept();
    void reject();
    void cancel();
    // Exact duplicate reports return false. Conflicting execution IDs are rejected.
    bool apply(const Fill& fill);
    [[nodiscard]] OrderState state() const noexcept { return state_; }
    [[nodiscard]] Decimal filled_quantity() const noexcept { return filled_; }
    [[nodiscard]] Decimal remaining_quantity() const { return request_.quantity - filled_; }
    [[nodiscard]] const LimitOrder& request() const noexcept { return request_; }

private:
    LimitOrder request_;
    Instrument spec_;
    OrderState state_ = OrderState::pending;
    Decimal filled_;
    std::set<Fill> fills_;
};
} // namespace asterion
