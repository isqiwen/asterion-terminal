#include "margins.hpp"
#include <asterion/base/serialization.hpp>
#include <asterion/system/process/artifact.hpp>
#include <algorithm>

namespace asterion::shfe {

void validate_scope(const std::string& venue, const std::string& day)
{
    if (venue != "SHFE" || std::ranges::find(days, day) == days.end())
        throw std::invalid_argument(
            "SHFE margin supports AU2512 on 2025-10-20 through 2025-10-22 only");
}

std::string report_path(const std::string& day, bool settlement)
{
    validate_scope("SHFE", day);
    auto compact = day;
    std::erase(compact, '-');
    return "/data/busiparamdata/future/"
        + std::string(settlement ? "Settlement" : "ContractDailyTradeArgument") + compact + ".dat";
}

MarginReport parse_report(const std::string& body, const std::string& day, bool settlement)
{
    validate_scope("SHFE", day);
    const auto document = parse_json(body, max_source_response_bytes);
    const auto key = settlement ? "Settlement" : "ContractDailyTradeArgument";
    auto compact = day;
    std::erase(compact, '-');
    if (!document.is_object() || document.size() != 3 || document.at("report_date") != compact
        || !document.at(key).is_array() || !document.at("update_date").is_string())
        throw std::invalid_argument("invalid SHFE margin report");
    MarginReport result{
        settlement ? "settlement" : "trading_day_start",
        "https://www.shfe.com.cn" + report_path(day, settlement),
        document.at("update_date"),
        sha256_bytes(body),
        {},
        {}
    };
    if (result.updated.size() != 17 || result.updated.substr(0, 8) != compact)
        throw std::invalid_argument("invalid SHFE margin report");
    unsigned found = 0;
    for (const auto& row : document.at(key)) {
        if (row.at("INSTRUMENTID") != "au2512")
            continue;
        ++found;
        if (row.at("TRADINGDAY") != compact)
            throw std::invalid_argument("invalid SHFE margin report");
        result.long_rate = Decimal::parse(row.at("SPEC_LONGMARGINRATIO").get<std::string>());
        result.short_rate = Decimal::parse(row.at("SPEC_SHORTMARGINRATIO").get<std::string>());
    }
    if (found != 1 || result.long_rate <= Decimal{} || result.short_rate <= Decimal{}
        || result.long_rate >= Decimal::parse("1") || result.short_rate >= Decimal::parse("1"))
        throw std::invalid_argument("invalid SHFE margin report");
    return result;
}

std::array<MarginReport, 2> reports(const SourceResponse& response, const std::string& day)
{
    if (response.format != response_format || response.received_at_ns <= 0)
        throw std::invalid_argument("invalid SHFE margin report");
    const auto raw = parse_json(response.body, max_source_response_bytes);
    if (!raw.is_object() || raw.size() != 2)
        throw std::invalid_argument("invalid SHFE margin report");
    return {
        parse_report(raw.at("trading_day_start").get<std::string>(), day, false),
        parse_report(raw.at("settlement").get<std::string>(), day, true)
    };
}

std::vector<TableSourceRow> rows(const SourceResponse& response, const std::string& day)
{
    std::vector<TableSourceRow> result;
    for (const auto& report : reports(response, day))
        result.push_back(
            {std::string(contract), report.phase, report.long_rate.str(), report.short_rate.str()});
    return result;
}

} // namespace asterion::shfe
