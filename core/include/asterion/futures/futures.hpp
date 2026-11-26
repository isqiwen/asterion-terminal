#pragma once
#include <asterion/futures/market.hpp>
#include <string_view>

namespace asterion {

bool is_dated_futures_symbol(
    std::string_view venue,
    std::string_view product,
    std::string_view symbol);

// Physical-delivery futures (all commodity venues and CFFEX treasury bonds)
// prohibit natural person accounts from opening positions in or after the delivery month.
bool prohibits_natural_person_delivery_month(std::string_view venue, std::string_view product);

// Chinese exchange-traded futures contract or product-wide policy entry
// (empty delivery_month with symbol == product).
struct FuturesContract
{
    void validate() const;
    bool matches(const InstrumentId& id) const;
    Instrument terms_for(const InstrumentId& id) const;
    std::string delivery_month_for(const InstrumentId& id, std::string_view trading_day) const;

    Instrument instrument;
    std::string product;
    std::string delivery_month; // YYYY-MM, or empty for product-wide policy entries.
};

} // namespace asterion
