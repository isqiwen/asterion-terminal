#include "guotai.hpp"
#include "report_html.hpp"
#include "values.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <regex>
#include <set>

namespace asterion::warehouse::acquisition {
namespace {

using html::first_child_tag;
using html::HtmlDocument;
using html::is_tag;
using html::text;

struct CollectionDeleter
{
    void operator()(lxb_dom_collection_t* collection) const noexcept
    {
        lxb_dom_collection_destroy(collection, true);
    }
};

using DomCollection = std::unique_ptr<lxb_dom_collection_t, CollectionDeleter>;

SourceError invalid_report(const std::string& message)
{
    return {"invalid_data", "invalid Guotai price tick report: " + message};
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
        for (auto pos = content.find("<table"); pos != std::string::npos;
            pos = content.find("<table", pos)) {
            const auto close = content.find("</table>", pos);
            if (close == std::string::npos)
                throw invalid_report("incomplete price tick table");
            pos = close + 8;
        }
        HtmlDocument document(lxb_html_document_create(), &lxb_html_document_destroy);
        if (!document
            || lxb_html_document_parse(
                   document.get(),
                   reinterpret_cast<const lxb_char_t*>(content.data()),
                   content.size())
                != LXB_STATUS_OK)
            throw invalid_report("invalid price tick table");
        DomCollection tables(
            lxb_dom_collection_make(lxb_dom_interface_document(document.get()), 8));
        if (!tables || !document->body
            || lxb_dom_elements_by_tag_name(
                   lxb_dom_interface_element(document->body),
                   tables.get(),
                   reinterpret_cast<const lxb_char_t*>("table"),
                   5)
                != LXB_STATUS_OK)
            throw invalid_report("invalid price tick table");
        for (std::size_t index = 0; index < lxb_dom_collection_length(tables.get()); ++index) {
            auto* table = lxb_dom_collection_node(tables.get(), index);
            auto* head = first_child_tag(table, LXB_TAG_THEAD);
            std::map<std::string, std::size_t> columns;
            if (head)
                for (auto* row = head->first_child; row; row = row->next) {
                    if (!is_tag(row, LXB_TAG_TR))
                        continue;
                    columns.clear();
                    for (auto* cell = row->first_child; cell; cell = cell->next)
                        if (cell->type == LXB_DOM_NODE_TYPE_ELEMENT)
                            columns.emplace(text(cell), columns.size());
                }
            if (!columns.contains("最小变动价位"))
                continue;
            auto* body = first_child_tag(table, LXB_TAG_TBODY);
            if (!body)
                throw invalid_report("missing price tick rows");
            for (auto* row = body->first_child; row; row = row->next) {
                if (!is_tag(row, LXB_TAG_TR))
                    continue;
                std::vector<std::string> cells;
                for (auto* cell = row->first_child; cell; cell = cell->next)
                    if (is_tag(cell, LXB_TAG_TD))
                        cells.push_back(text(cell));
                if (cells.size() != columns.size())
                    throw invalid_report("wrong price tick columns");
                const auto field = [&](const char* name) -> const std::string& {
                    return cells.at(columns.at(name));
                };
                auto product = field("代码");
                const auto exchange = exchanges.find(field("交易所"));
                if (exchange == exchanges.end() || !std::regex_match(product, future)
                    || field("品种").find("期权") != std::string::npos)
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

Guotai::Guotai(
    std::vector<std::string> exchanges,
    std::shared_ptr<Budget> budget,
    std::stop_token stop,
    Fetch fetch)
    : exchanges_(std::move(exchanges)),
      budget_(std::move(budget)),
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
        budget_->acquire(stop_);
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
