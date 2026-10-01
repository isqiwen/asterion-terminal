#pragma once
#include <asterion/domain/order.hpp>
#include <asterion/foundation/serialization.hpp>
#include <optional>
#include <vector>
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
// One contract of a portfolio: its specification and its cost schedule.
struct ContractTerms {
  Instrument instrument;
  FuturesCosts costs;
};
inline constexpr std::size_t max_portfolio_contracts = 20;
struct PositionLot {
  InstrumentId instrument;
  Side side;
  bool today;
  Decimal quantity, price;
};
struct AccountOrder {
  Order order;
  Offset offset;
};
// Single-currency futures portfolio ledger over a fixed set of contracts.
// Funds, margin, frozen amounts and fees are account-wide; positions, marks
// and close rules belong to each contract. Host serializes access. Mutations
// have a strong exception guarantee; report IDs stay unique for the ledger.
// Typed queries are the internal API; snapshot() presents the same state.
class FuturesAccount {
public:
  FuturesAccount(Decimal deposit, std::vector<ContractTerms> contracts);
  void submit(LimitOrder request, Offset offset);
  void cancel(const std::string& id);
  bool fill(const Fill& report);
  void mark(const InstrumentId& instrument, Decimal price);
  // Settles every contract at its price, ordered as contracts(). Only with no
  // outstanding orders; the caller owns the calendar.
  void settle(const std::vector<Decimal>& prices);
  Json snapshot() const;
  const std::vector<ContractTerms>& contracts() const noexcept { return contracts_; }
  // Index of a contract in contracts(); throws for a contract outside the account.
  std::size_t contract_index(const InstrumentId& instrument) const;
  const std::vector<AccountOrder>& orders() const noexcept { return orders_; }
  const std::vector<PositionLot>& positions() const noexcept { return lots_; }
  const std::vector<Fill>& fills() const noexcept { return fills_; }
  Decimal balance() const noexcept { return balance_; }
  Decimal fees() const noexcept { return fees_; }
  Decimal realized() const noexcept { return realized_; }
  // Zero until the contract's first mark; its orders are rejected before that.
  Decimal last_mark(const InstrumentId& instrument) const;
  Decimal available() const;
  Decimal unrealized() const;
  Decimal margin() const;
  Decimal frozen() const;
  bool has_working_orders() const noexcept;
  ClosePolicy close_policy(const InstrumentId& instrument) const;

private:
  // Quantity of position_side still free to close in one contract; `today`
  // limits it to one bucket, nullopt counts both (generic close).
  Decimal closable(const InstrumentId& instrument, Side position_side,
                   std::optional<bool> today) const;
  // Reservation for the rest of a working order at its limit price.
  Decimal reserved(const AccountOrder& item) const;
  std::size_t index_of(const std::string& id) const;
  std::vector<ContractTerms> contracts_;
  std::vector<Decimal> marks_;
  Decimal balance_, fees_, realized_;
  std::vector<PositionLot> lots_;
  std::vector<AccountOrder> orders_;
  std::vector<Fill> fills_;
  // Identity indexes into orders_ and fills_; both vectors only grow.
  std::unordered_map<std::string, std::size_t> order_index_;
  std::unordered_map<std::string, std::size_t> fill_index_;
};
} // namespace asterion
