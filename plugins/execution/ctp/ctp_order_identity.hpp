#pragma once
#include <compare>
#include <cstdint>
#include <map>
#include <string>
namespace asterion::ctp {
// Front/session/reference identifies an order only within its trading day.
struct OrderIdentity {
  std::string trading_day, broker_key;
  auto operator<=>(const OrderIdentity&) const = default;
};
struct KnownOrder {
  std::string order_id;
  std::uint64_t sequence;
};
using KnownOrders = std::map<OrderIdentity, KnownOrder>;
} // namespace asterion::ctp
