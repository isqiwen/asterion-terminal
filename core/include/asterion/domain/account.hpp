#pragma once
#include <asterion/domain/order.hpp>
#include <asterion/foundation/serialization.hpp>
#include <vector>

namespace asterion {
enum class Offset { open, close_today, close_yesterday };
struct FuturesCosts {
  Decimal margin_per_lot, open_fee, close_today_fee, close_yesterday_fee;
  void validate() const;
  Decimal fee(Offset offset) const;
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
  const std::vector<AccountOrder>& orders() const { return orders_; }
  const std::vector<PositionLot>& positions() const { return lots_; }
  const Instrument& instrument() const { return instrument_; }

private:
  void submit_in_place(LimitOrder request, Offset offset);
  void fill_in_place(const Fill& report);
  Decimal available() const;
  Decimal unrealized() const;
  Decimal margin() const;
  Decimal frozen() const;
  Decimal closable(Side position_side, bool today) const;
  AccountOrder& find(const std::string& id);
  Instrument instrument_;
  FuturesCosts costs_;
  Decimal balance_, fees_, realized_, mark_;
  std::vector<PositionLot> lots_;
  std::vector<AccountOrder> orders_;
  std::vector<Fill> fills_;
};
} // namespace asterion
