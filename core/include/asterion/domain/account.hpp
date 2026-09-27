#pragma once
#include <asterion/domain/order.hpp>
#include <asterion/foundation/serialization.hpp>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace asterion {
// close lets the exchange assign the order to today's/yesterday's positions;
// close_today/close_yesterday name the bucket explicitly.
enum class Offset { open, close_today, close_yesterday, close };
// How an exchange assigns a close order to today's and yesterday's positions.
// SHFE/INE require explicit buckets; CFFEX closes today's first; DCE, CZCE and
// GFEX close yesterday's first. Verify against current exchange rules before
// relying on a venue; unknown venues require explicit buckets.
enum class ClosePolicy { explicit_buckets, today_first, yesterday_first };
ClosePolicy close_policy(std::string_view venue) noexcept;
// Costs have a per-lot part and a notional part (price x quantity x
// multiplier x rate). Notional parts round half-up to 0.01 in the quote
// currency; per-lot parts are exact. Rates default to 0 (per-lot only).
struct FuturesCosts {
  Decimal margin_per_lot, open_fee, close_today_fee, close_yesterday_fee;
  Decimal margin_rate, open_fee_rate, close_today_fee_rate, close_yesterday_fee_rate;
  void validate() const;
  // bucket is open, close_today or close_yesterday (never the generic close).
  Decimal fee(Offset bucket, Decimal quantity, Decimal price, Decimal multiplier) const;
  Decimal margin(Decimal quantity, Decimal price, Decimal multiplier) const;
};
struct PositionLot {
  Side side;
  bool today;
  Decimal quantity, price;
};
struct AccountOrder {
  Order order;
  Offset offset;
};
// Single-currency, single-contract ledger. Host serializes access. Mutations have
// a strong exception guarantee; report IDs remain unique for the ledger lifetime.
// Typed queries are the internal API; snapshot() is a presentation of the same state.
class FuturesAccount {
public:
  FuturesAccount(Instrument instrument, Decimal deposit, FuturesCosts costs);
  void submit(LimitOrder request, Offset offset);
  void cancel(const std::string& id);
  bool fill(const Fill& report);
  void mark(Decimal price);
  // Explicit settlement, only with no outstanding orders. Caller owns calendar.
  void settle(Decimal price);
  Json snapshot() const;
  const std::vector<AccountOrder>& orders() const noexcept { return orders_; }
  const std::vector<PositionLot>& positions() const noexcept { return lots_; }
  const std::vector<Fill>& fills() const noexcept { return fills_; }
  const Instrument& instrument() const noexcept { return instrument_; }
  const FuturesCosts& costs() const noexcept { return costs_; }
  Decimal balance() const noexcept { return balance_; }
  Decimal fees() const noexcept { return fees_; }
  Decimal realized() const noexcept { return realized_; }
  // Zero until the first mark; the ledger rejects orders before that.
  Decimal last_mark() const noexcept { return mark_; }
  Decimal available() const;
  Decimal unrealized() const;
  Decimal margin() const;
  Decimal frozen() const;
  bool has_working_orders() const noexcept;
  ClosePolicy close_policy() const noexcept { return policy_; }

private:
  // Quantity of position_side still free to close; `today` limits it to one
  // bucket, nullopt counts both (generic close).
  Decimal closable(Side position_side, std::optional<bool> today) const;
  // Reservation for the rest of a working order at its limit price.
  Decimal reserved(const AccountOrder& item) const;
  std::size_t index_of(const std::string& id) const;
  Instrument instrument_;
  FuturesCosts costs_;
  ClosePolicy policy_;
  Decimal balance_, fees_, realized_, mark_;
  std::vector<PositionLot> lots_;
  std::vector<AccountOrder> orders_;
  std::vector<Fill> fills_;
  // Identity indexes into orders_ and fills_; both vectors only grow.
  std::unordered_map<std::string, std::size_t> order_index_;
  std::unordered_map<std::string, std::size_t> fill_index_;
};
} // namespace asterion
