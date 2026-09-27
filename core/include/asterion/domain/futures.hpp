#pragma once
#include <asterion/domain/market.hpp>

namespace asterion {
// Initial supported market: Chinese exchange-traded futures with explicit
// delivery month. This is contract identity, not an exchange rule/calendar feed.
struct FuturesContract {
  Instrument instrument;
  std::string product;
  std::string delivery_month; // YYYY-MM; never infer the decade from a symbol.
  void validate() const;
};
} // namespace asterion
