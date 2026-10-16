#pragma once
#include <asterion/domain/history_tables.hpp>
#include <asterion/domain/source_response.hpp>
#include <asterion/foundation/decimal.hpp>
#include <array>

namespace asterion::shfe {

inline constexpr auto provider = "asterion.data.shfe";
inline constexpr auto source = "shfe.au2512.margin";
inline constexpr auto table = "futures.margin";
inline constexpr auto response_format = "shfe.margin.reports";
inline constexpr auto contract = "SHFE/au/2025-12";
inline constexpr std::array days{"2025-10-20", "2025-10-21", "2025-10-22"};

struct MarginReport
{
    std::string phase, url, updated, sha256;
    Decimal long_rate, short_rate;
};

void validate_scope(const std::string& venue, const std::string& day);
std::string report_path(const std::string& day, bool settlement);
MarginReport parse_report(const std::string& body, const std::string& day, bool settlement);
std::array<MarginReport, 2> reports(const SourceResponse&, const std::string& day);
std::vector<TableSourceRow> rows(const SourceResponse&, const std::string& day);

} // namespace asterion::shfe
