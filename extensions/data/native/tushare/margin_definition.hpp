#pragma once
#include <asterion/domain/historical_margin.hpp>

namespace asterion::tushare {

// Official fut_settle documentation names speculative sides but does not establish
// exchange-specific units or the intraday effective-time mapping. Remain blocked.
inline historical_costs::MarginDefinition settlement_margin_definition(const std::string& exchange)
{
    historical_costs::MarginDefinition definition;
    definition.source = "tushare.fut_settle.v2";
    definition.exchange = exchange;
    definition.basis = historical_costs::MarginBasis::exchange_speculation;
    definition.basis_reference = "https://tushare.pro/document/2?doc_id=141";
    return definition;
}

} // namespace asterion::tushare
