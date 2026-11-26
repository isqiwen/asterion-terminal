#pragma once
#include "exchanges.hpp"
#include "values.hpp"
#include <set>
#include <string>
#include <vector>

namespace asterion::warehouse::acquisition::detail {

std::string trim(std::string text);
std::string compact(std::string day);
std::string text(const values::Json& value);
warehouse::Cell number(const values::Json& value, int shift = 0);
std::string utf8(const std::string& raw);
DailyBar daily_row(
    const std::string& exchange,
    const std::string& contract,
    const std::string& day,
    warehouse::Cell open,
    warehouse::Cell high,
    warehouse::Cell low,
    warehouse::Cell close,
    warehouse::Cell settle,
    warehouse::Cell previous,
    warehouse::Cell volume,
    warehouse::Cell interest);
void unique_contract(std::set<std::string>& seen, const std::string& contract);

Exchange::Parsed parse_shanghai(
    const std::string& exchange,
    const std::string& day,
    const std::string& table,
    const std::string& raw);

std::vector<PriceTick>
parse_czce_price_ticks(const std::string& day, const std::string& raw, const std::string& url);

Exchange::Parsed parse_text_rows(
    const std::string& exchange,
    const std::string& day,
    const std::string& table,
    const std::string& raw,
    bool archive = false);

Exchange::Parsed parse_portal(
    const std::string& exchange,
    const std::string& day,
    const std::string& table,
    const std::string& raw);

} // namespace asterion::warehouse::acquisition::detail
