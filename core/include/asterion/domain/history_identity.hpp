#pragma once
#include <asterion/domain/market.hpp>
#include <optional>
#include <vector>
#include <stop_token>
namespace asterion {
// Stable history identity. Exchange symbols alone may repeat across decades.
struct HistoryIdentity {
  std::string venue, product, delivery_month;
  auto operator<=>(const HistoryIdentity&) const = default;
  void validate() const;
  std::string key() const;
  InstrumentId exchange_id() const;
  static HistoryIdentity parse(std::string_view);
};
struct HistoryListing {
  HistoryIdentity identity;
  std::string name, list_date, delist_date;
  std::string source_instrument = {};
  std::optional<Decimal> multiplier = {}, per_unit = {};
  std::optional<std::string> trade_unit = {}, quote_unit = {};
};
// Serialized with each dataset. Unknown label semantics cannot be used for execution.
struct HistorySemantics {
  std::string source, normalization, timezone, timestamp_semantics;
  std::string amount_unit = "quote_currency", quantity_unit = "contracts";
  auto operator<=>(const HistorySemantics&) const = default;
  void validate() const;
};
void validate_history_source(std::string_view);
std::int64_t parse_shanghai_time(std::string_view);
std::string format_shanghai_time(std::int64_t);
} // namespace asterion
