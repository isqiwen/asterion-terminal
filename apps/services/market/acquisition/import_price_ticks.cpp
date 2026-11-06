#include "manager.hpp"
#include "warehouse/research.hpp"
#include <asterion/protocol/common.hpp>
#include <algorithm>
#include <regex>

namespace asterion::market_data::acquisition {

wire::Dataset Manager::import_price_ticks(const wire::ImportPriceTicks& request)
{
    require_exchange(request.exchange());
    if (request.rows().empty())
        throw std::invalid_argument("price tick import requires at least one rule");
    Rows rows;
    for (const auto& rule : request.rows()) {
        require_contract(rule.contract());
        if (!rule.contract().starts_with(request.exchange() + '/'))
            throw std::invalid_argument(
                "price tick import contains another exchange: " + rule.contract());
        const auto first = day_of(rule.effective_from()), until = day_of(rule.effective_until());
        if (first >= until)
            throw std::invalid_argument("invalid price tick interval: " + rule.contract());
        const auto tick = protocol::decimal(rule.price_tick());
        if (tick <= Decimal{})
            throw std::invalid_argument("price tick must be positive: " + rule.contract());
        static const std::regex url(R"(https?://[^\s]+)");
        if (!std::regex_match(rule.source_url(), url))
            throw std::invalid_argument(
                "price tick rule requires a source URL: " + rule.contract());
        const auto start = request.exchange().size() + 1;
        const auto product =
            rule.contract().substr(start, rule.contract().find('/', start) - start);
        rows.push_back(
            {request.exchange(),
                product,
                rule.contract(),
                rule.effective_from(),
                rule.effective_until(),
                tick,
                rule.source_url()});
    }
    std::ranges::sort(rows, [](const auto& left, const auto& right) {
        return std::tie(left[2], left[3]) < std::tie(right[2], right[3]);
    });
    for (std::size_t i = 1; i < rows.size(); ++i)
        if (rows[i - 1][2] == rows[i][2] && rows[i - 1][4] > rows[i][3])
            throw std::invalid_argument(
                "overlapping imported price tick rules: " + std::get<std::string>(rows[i][2]));
    auto writer = rules_.writer();
    writer.put("price_ticks/" + request.exchange() + "/history", rows);
    const auto version = writer.commit();
    warehouse::compose_research(directory_.parent_path(), research_);
    wire::Dataset result;
    result.set_provider(version.provider);
    result.set_version(version.identity);
    result.set_created_ms(version.created_ns / 1'000'000);
    return result;
}

} // namespace asterion::market_data::acquisition
