#include "guotai.hpp"
#include "values.hpp"
#include <tinyxml2.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>

namespace asterion::warehouse::acquisition {
namespace {

SourceError invalid_report(const std::string& message)
{
    return {"invalid_data", "invalid Guotai price tick report: " + message};
}

std::string text(const tinyxml2::XMLNode* node)
{
    std::string result;
    for (auto* child = node->FirstChild(); child; child = child->NextSibling())
        result += child->ToText() ? child->Value() : text(child);
    const auto begin = result.find_first_not_of(" \t\r\n");
    return begin == std::string::npos
        ? std::string{}
        : result.substr(begin, result.find_last_not_of(" \t\r\n") - begin + 1);
}

std::vector<PriceTick> parse(const std::string& day, const std::string& raw, const std::string& url)
{
    const auto script = raw.find("id=\"__NEXT_DATA__\"");
    const auto begin = raw.find('>', script), end = raw.find("</script>", begin);
    if (script == std::string::npos || begin == std::string::npos || end == std::string::npos)
        throw invalid_report("missing dated calendar data");
    const auto page = values::parse(raw.substr(begin + 1, end - begin - 1));
    const auto& data = page.at("props").at("pageProps").at("DEFAULT_CONTENT");
    auto date = day;
    std::erase(date, '-');
    if (data.at("tradingday") != date)
        throw invalid_report("wrong price tick report date");
    if (data.at("tradeflag") != "T")
        return {};
    const auto until = day_text(day_of(day) + std::chrono::days(1));
    const std::map<std::string, std::string> exchanges{
        {"上期所", "SHFE"},
        {"能源中心", "INE"},
        {"大商所", "DCE"},
        {"中金所", "CFFEX"},
        {"广期所", "GFEX"}
    };
    const std::regex future("[A-Z]+");
    std::set<std::pair<std::string, std::string>> seen;
    std::vector<PriceTick> rows;
    for (const auto& event : data.at("events")) {
        if (event.at("event_date") != date)
            throw invalid_report("wrong price tick event date");
        const auto content = event.at("content").get<std::string>();
        for (auto first = content.find("<table"); first != std::string::npos;
            first = content.find("<table", first)) {
            const auto last = content.find("</table>", first);
            if (last == std::string::npos)
                throw invalid_report("incomplete price tick table");
            tinyxml2::XMLDocument document;
            if (document.Parse(content.data() + first, last + 8 - first) != tinyxml2::XML_SUCCESS)
                throw invalid_report("invalid price tick table");
            first = last + 8;
            const auto* table = document.RootElement();
            const auto* head = table->FirstChildElement("thead");
            std::map<std::string, std::size_t> columns;
            if (head)
                for (auto* row = head->FirstChildElement("tr"); row;
                    row = row->NextSiblingElement("tr")) {
                    columns.clear();
                    for (auto* cell = row->FirstChildElement(); cell;
                        cell = cell->NextSiblingElement())
                        columns.emplace(text(cell), columns.size());
                }
            if (!columns.contains("最小变动价位"))
                continue;
            const auto* body = table->FirstChildElement("tbody");
            if (!body)
                throw invalid_report("missing price tick rows");
            for (auto* row = body->FirstChildElement("tr"); row;
                row = row->NextSiblingElement("tr")) {
                std::vector<std::string> cells;
                for (auto* cell = row->FirstChildElement("td"); cell;
                    cell = cell->NextSiblingElement("td"))
                    cells.push_back(text(cell));
                if (cells.size() != columns.size())
                    throw invalid_report("wrong price tick columns");
                const auto field = [&](const char* name) -> const std::string& {
                    return cells.at(columns.at(name));
                };
                auto product = field("代码");
                const auto exchange = exchanges.find(field("交易所"));
                if (exchange == exchanges.end() || !std::regex_match(product, future))
                    continue;
                for (const auto* name : {"特殊合约参数调整", "调整备注"})
                    if (field(name).find("最小变动价位") != std::string::npos)
                        throw invalid_report("contract-specific price tick adjustment: " + product);
                const auto tick = Decimal::parse(field("最小变动价位"));
                if (tick <= Decimal{})
                    throw invalid_report("nonpositive historical price tick: " + product);
                std::transform(
                    product.begin(),
                    product.end(),
                    product.begin(),
                    [](unsigned char c) {
                        return std::tolower(c);
                    });
                if (!seen.emplace(exchange->second, product).second)
                    throw invalid_report("duplicate historical product price tick: " + product);
                rows.push_back(
                    {.exchange = exchange->second,
                        .product = product,
                        .effective_from = day,
                        .effective_until = until,
                        .price_tick = tick,
                        .source_url = url});
            }
        }
    }
    return rows;
}

} // namespace

Guotai::Guotai(std::vector<std::string> exchanges, std::stop_token stop, Fetch fetch)
    : exchanges_(std::move(exchanges)),
      stop_(stop),
      fetch_(std::move(fetch))
{}

bool Guotai::supports(std::string_view table, std::string_view exchange) const
{
    return table == "price_ticks" && exchange != "CZCE"
        && std::find(exchanges_.begin(), exchanges_.end(), exchange) != exchanges_.end();
}

std::vector<PriceTick> Guotai::price_ticks(const std::string& exchange, const std::string& day)
{
    check_stop(stop_);
    if (day_ != day) {
        auto date = day;
        std::erase(date, '-');
        const HttpRequest request{"https://www.gtjaqh.com", "/pc/calendar?date=" + date, {}, {}};
        ++requests_;
        const auto reply = fetch_(request, stop_);
        check_stop(stop_);
        if (reply.status != 200 && reply.status != 404)
            throw SourceError(
                "unavailable",
                "Guotai report refused (HTTP " + std::to_string(reply.status) + ")");
        rows_ = reply.status == 404 ? std::vector<PriceTick>{}
                                    : parse(day, reply.body, request.origin + request.path);
        day_ = day;
    }
    std::vector<PriceTick> result;
    for (const auto& row : rows_)
        if (row.exchange == exchange)
            result.push_back(row);
    return result;
}

} // namespace asterion::warehouse::acquisition
