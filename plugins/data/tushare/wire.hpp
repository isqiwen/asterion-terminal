#pragma once
#include "tushare.hpp"
// What every Tushare reader does with a response, whatever the interface.
namespace asterion::tushare {
// Parses a response keeping every number as the text the vendor sent, so no
// value passes through binary floating point.
Json parse_response(const std::string& raw);
// Why the vendor refused a request, from its documented codes and wording.
AccessFailure rejection(const Json& response);
std::string text(const Json& value);
// Rejects a vendor code that does not name this contract.
void mapping(const HistoryIdentity& identity, const std::string& provider_code);
// A number's text in plain notation: expands an exponent and moves the point
// `shift` places to the right, without rounding.
std::string plain_decimal(const Json& value, int shift = 0);
} // namespace asterion::tushare
