#include "exchange_parsers.hpp"
#include "report_html.hpp"
#include <asterion/base/decimal.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <map>
#include <regex>
#include <set>
#include <span>

namespace asterion::warehouse::acquisition::detail {
namespace {

using values::Json;
using warehouse::Cell;
using Parsed = Exchange::Parsed;
const std::set<std::string> ine_products{"bc", "ec", "lu", "nr", "sc"};

Cell shanghai_published(const Json& root)
{
    Cell published;
    if (root.contains("update_date")) {
        const auto label = text(root.at("update_date"));
        // 能源中心多年的原报在日期和时间之间没有空格。
        static const std::regex clock(
            "([0-9]{4})([0-9]{2})([0-9]{2}) ?([0-9]{2}):([0-9]{2}):([0-9]{2})");
        std::smatch at;
        if (!std::regex_match(label, at, clock))
            throw std::invalid_argument("invalid exchange report update time");
        const auto hour = std::stoi(at.str(4)), minute = std::stoi(at.str(5)),
                   second = std::stoi(at.str(6));
        if (hour > 23 || minute > 59 || second > 59)
            throw std::invalid_argument("invalid exchange report update time");
        const auto date = at.str(1) + "-" + at.str(2) + "-" + at.str(3);
        published = std::chrono::duration_cast<std::chrono::milliseconds>(
            (day_of(date) + std::chrono::hours(hour - 8) + std::chrono::minutes(minute)
                + std::chrono::seconds(second))
                .time_since_epoch())
                        .count();
    }
    return published;
}

std::string shanghai_label(const Json& value)
{
    const auto both = text(value);
    return both.substr(0, both.find("$$"));
}

std::string shanghai_unit(const Json& value)
{
    static const std::map<std::string, std::string> units{{"1", "千克"}, {"2", "吨"}, {"3", "桶"}};
    return units.at(text(value));
}

std::pair<std::string, Cell> shanghai_product(std::string name, const std::string& code = {})
{
    static const std::map<std::string, std::string> products{
        {"铜", "cu"},
        {"铝", "al"},
        {"锌", "zn"},
        {"铅", "pb"},
        {"镍", "ni"},
        {"锡", "sn"},
        {"黄金", "au"},
        {"白银", "ag"},
        {"螺纹钢", "rb"},
        {"线材", "wr"},
        {"热轧卷板", "hc"},
        {"沥青", "bu"},
        {"石油沥青", "bu"},
        {"天然橡胶", "ru"},
        {"燃料油", "fu"},
        {"中质含硫原油", "sc"},
        {"20号胶", "nr"},
        {"纸浆", "sp"},
        {"不锈钢", "ss"},
        {"低硫燃料油", "lu"},
        {"铜(BC)", "bc"},
        {"氧化铝", "ao"},
        {"丁二烯橡胶", "br"},
        {"铸造铝合金", "ad"},
        {"胶版印刷纸", "op"}
    };
    Cell warehouse_type;
    for (const std::string_view suffix : {"仓库", "厂库"}) {
        const auto at = name.find(suffix);
        if (at != std::string::npos) {
            warehouse_type = std::string(suffix);
            name.resize(at);
            if (name.ends_with('('))
                name.pop_back();
        }
    }
    return {code.empty() ? products.at(name) : code, warehouse_type};
}

std::pair<std::string, Cell> shanghai_product(const Json& row)
{
    return shanghai_product(
        shanghai_label(row.at("VARNAME")),
        row.contains("VARID") ? text(row.at("VARID")) : "");
}

Cell previous_receipts(const Cell& volume, const Cell& change)
{
    if (volume == Cell{} || change == Cell{})
        return {};
    return Decimal::parse(std::get<std::string>(volume))
        - Decimal::parse(std::get<std::string>(change));
}

std::vector<WarehouseReceipt>
shanghai_receipts(const std::string& exchange, const std::string& day, const Json& root)
{
    const auto published = shanghai_published(root);
    const auto observed = observation_time();
    std::vector<WarehouseReceipt> result;
    for (const auto& row : root.at("o_cursor")) {
        if (text(row.at("ROWSTATUS")) != "0")
            continue;
        const auto [product, warehouse_type] = shanghai_product(row);
        if (ine_products.contains(product) != (exchange == "INE"))
            continue;
        const auto volume = number(row.at("WRTWGHTS")), change = number(row.at("WRTCHANGE"));
        const auto area = shanghai_label(row.at("REGNAME"));
        result.push_back(
            {.exchange = exchange,
                .product = product,
                .day = day,
                .warehouse = shanghai_label(row.at("WHABBRNAME")),
                .warehouse_type = warehouse_type,
                .area = area.empty() ? Cell{} : Cell{area},
                .unit = shanghai_unit(row.at("WGHTUNIT")),
                .previous_volume = previous_receipts(volume, change),
                .volume = volume,
                .volume_change = change,
                .published_ms = published,
                .observed_ms = observed});
    }
    return result;
}

using Nodes = std::vector<lxb_dom_node_t*>;

Nodes children(lxb_dom_node_t* parent, lxb_tag_id_t tag)
{
    Nodes result;
    for (auto* child = parent ? parent->first_child : nullptr; child; child = child->next)
        if (html::is_tag(child, tag))
            result.push_back(child);
    return result;
}

std::string_view attribute(lxb_dom_node_t* node, std::string_view name)
{
    std::size_t size = 0;
    const auto* raw = lxb_dom_element_get_attribute(
        lxb_dom_interface_element(node),
        reinterpret_cast<const lxb_char_t*>(name.data()),
        name.size(),
        &size);
    return raw ? std::string_view(reinterpret_cast<const char*>(raw), size) : std::string_view{};
}

html::HtmlDocument shfe_document(const std::string& raw)
{
    html::HtmlDocument document(lxb_html_document_create(), &lxb_html_document_destroy);
    if (!document
        || lxb_html_document_parse(
               document.get(),
               reinterpret_cast<const lxb_char_t*>(raw.data()),
               raw.size())
            != LXB_STATUS_OK)
        throw std::invalid_argument("invalid SHFE report HTML");
    return document;
}

struct FirstColumn
{
    void expand(Nodes& cells)
    {
        if (remaining) {
            cells.insert(cells.begin(), shared);
            --remaining;
        } else if (const auto span = attribute(cells.at(0), "rowspan"); !span.empty()) {
            shared = cells.front();
            const auto [end, error] =
                std::from_chars(span.data(), span.data() + span.size(), remaining);
            if (error != std::errc{} || end != span.data() + span.size() || !remaining)
                throw std::invalid_argument("invalid SHFE report rowspan");
            --remaining;
        }
    }

    lxb_dom_node_t* shared{};
    unsigned remaining{};
};

std::vector<std::string> labels(const Nodes& cells)
{
    std::vector<std::string> result;
    for (auto* cell : cells)
        result.push_back(html::text(cell));
    while (!result.empty() && result.back().empty())
        result.pop_back();
    return result;
}

std::string inventory_label(std::string label)
{
    std::erase_if(label, [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    });
    for (const std::string_view space : {"\xC2\xA0", "　"})
        for (auto at = label.find(space); at != std::string::npos; at = label.find(space, at))
            label.erase(at, space.size());
    return label;
}

void fill_archive_region(
    std::span<Inventory> group,
    const Nodes& subtotal,
    std::size_t capacity_column)
{
    const Cell* area = nullptr;
    for (std::size_t i = group.size(); i > 0; --i) {
        const auto& region = group[i - 1].area;
        if (region == Cell{})
            continue;
        if (area && *area != region) {
            // 单列地区可没有合计，只有后续明细独占本组合计库容时才能补齐空地区。
            group = group.subspan(i);
            const auto capacity = number(html::text(subtotal.at(capacity_column)));
            if (capacity == Cell{})
                return;
            Decimal sum;
            for (const auto& row : group) {
                if (row.capacity == Cell{})
                    return;
                const auto value = Decimal::parse(std::get<std::string>(row.capacity));
                if (value <= Decimal{})
                    return;
                sum = sum + value;
            }
            if (sum != Decimal::parse(std::get<std::string>(capacity)))
                return;
            break;
        }
        area = &region;
    }
    if (area)
        for (auto& row : group)
            if (row.area == Cell{})
                row.area = *area;
}

void inventory_section(
    std::vector<Inventory>& result,
    const std::string& day,
    std::int64_t observed,
    const Nodes& title,
    const Nodes& headings,
    const Nodes& subheadings,
    std::span<lxb_dom_node_t* const> rows,
    bool archive)
{
    auto name = html::text(title.at(0));
    constexpr std::string_view prefix = "交割商品：";
    if (name.starts_with(prefix))
        name.erase(0, prefix.size());
    const auto [product, type] = shanghai_product(name);
    if (ine_products.contains(product))
        return;
    std::string unit;
    for (const auto& label : labels(title))
        if (label.starts_with("单位："))
            unit = label.substr(std::string_view("单位：").size());
    if (unit.empty())
        throw std::invalid_argument("missing SHFE inventory unit");
    const auto header = labels(headings);
    Inventory base{
        .exchange = "SHFE",
        .product = product,
        .day = day,
        .warehouse_type = type,
        .unit = unit,
        .observed_ms = observed
    };
    if (product == "au") {
        if (header != std::vector<std::string>{"上周库存", "本周库存", "库存增减"} || rows.empty())
            throw std::invalid_argument("unexpected SHFE gold inventory report");
        const auto cells = children(rows.front(), LXB_TAG_TD);
        base.warehouse = std::string("总计");
        base.previous_total = base.previous_receipts = number(html::text(cells.at(0)));
        base.total = base.receipts = number(html::text(cells.at(1)));
        base.total_change = base.receipts_change = number(html::text(cells.at(2)));
        result.push_back(std::move(base));
        return;
    }
    const auto area = header.at(0) == "地区" ? 0U : 1U, warehouse = 1 - area;
    const bool graded = header.at(2) == "品级";
    const auto first = graded ? 3U : 2U;
    const bool receipts_only = header.at(first) == "上周期货";
    const std::vector<std::string> sub = receipts_only
        ? std::vector<std::string>{"上周", "本周", "增减"}
        : std::vector<
              std::string>{"小计", "期货", "小计", "期货", "小计", "期货", "上周", "本周", "增减"};
    if (header.size() != first + 4 || header[area] != "地区"
        || (header[warehouse] != "仓库" && header[warehouse] != "厂库")
        || header[first] != (receipts_only ? "上周期货" : "上周库存")
        || header[first + 1] != (receipts_only ? "本周期货" : "本周库存")
        || header[first + 2] != (receipts_only ? "增减" : "库存增减")
        || (header.back() != "可用库容量" && header.back() != "理论可用库容量")
        || labels(subheadings) != sub)
        throw std::invalid_argument("unexpected SHFE inventory headings for " + name);
    FirstColumn leading;
    auto group = result.size();
    for (auto* row : rows) {
        auto cells = children(row, LXB_TAG_TD);
        const auto raw_label = html::text(cells.at(0));
        if (raw_label.starts_with("注") || raw_label.ends_with("标准仓单到期数量"))
            break;
        leading.expand(cells);
        const auto left = html::text(cells.front());
        const auto second = cells.size() > 1 ? html::text(cells[1]) : std::string{};
        const auto first_label = inventory_label(left), second_label = inventory_label(second);
        if (first_label == "总计")
            break;
        const auto aggregate = [](const std::string& label) {
            return label.ends_with("合计") || label.ends_with("总计") || label.ends_with("小计");
        };
        if (aggregate(first_label) || aggregate(second_label)
            || (graded && aggregate(inventory_label(html::text(cells.at(2)))))) {
            // 早期电子表把地区名放在一组明细的中间行，由该组的合计划定范围。
            if (archive && (first_label == "合计" || second_label == "合计"))
                fill_archive_region(
                    std::span(result).subspan(group),
                    cells,
                    (first_label == "合计" ? 1 : first) + (receipts_only ? 4 : 7));
            group = result.size();
            continue;
        }
        if (cells.size() < first + (receipts_only ? 6 : 9))
            throw std::invalid_argument("incomplete SHFE inventory row for " + name);
        auto detail = base;
        detail.warehouse = warehouse == 0 ? left : second;
        const auto& region = area == 0 ? left : second;
        if (!(area == 0 ? first_label : second_label).empty())
            detail.area = region;
        if (graded) {
            const auto grade = html::text(cells.at(2));
            if (!grade.empty())
                detail.grade = grade;
        }
        auto column = first;
        const auto next = [&] {
            return number(html::text(cells.at(column++)));
        };
        if (!receipts_only)
            detail.previous_total = next();
        detail.previous_receipts = next();
        if (!receipts_only)
            detail.total = next();
        detail.receipts = next();
        if (!receipts_only)
            detail.total_change = next();
        detail.receipts_change = next();
        detail.previous_capacity = next();
        detail.capacity = next();
        detail.capacity_change = next();
        result.push_back(std::move(detail));
    }
    if (leading.remaining)
        throw std::invalid_argument("incomplete SHFE inventory rowspan");
}

std::vector<Inventory> shanghai_inventory(const std::string& day, const Json& root)
{
    const auto published = shanghai_published(root);
    const auto observed = observation_time();
    std::vector<Inventory> result;
    for (const auto* cursor : {"o_cursor", "o_cursorfu"}) {
        if (!root.contains(cursor) || root.at(cursor).is_null())
            continue;
        if (!root.at(cursor).is_array())
            throw std::invalid_argument("expected exchange rows");
        for (const auto& row : root.at(cursor)) {
            if (text(row.at("ROWSTATUS")) != "0")
                continue;
            const auto [product, warehouse_type] = shanghai_product(row);
            if (product == "au" || ine_products.contains(product))
                continue;
            const auto area = shanghai_label(row.at("REGNAME"));
            const auto grade = row.contains("GRDNAME") && !row.at("GRDNAME").is_null()
                ? shanghai_label(row.at("GRDNAME"))
                : std::string{};
            const auto n = [&](const char* key) {
                return number(row.at(key));
            };
            result.push_back(
                {.exchange = "SHFE",
                    .product = product,
                    .day = day,
                    .area = area.empty() ? Cell{} : Cell{area},
                    .warehouse = shanghai_label(row.at("WHABBRNAME")),
                    .warehouse_type = warehouse_type,
                    .grade = grade.empty() ? Cell{} : Cell{grade},
                    .unit = shanghai_unit(row.at("WGHTUNIT")),
                    .previous_total = n("PRESPOTWGHTS"),
                    .previous_receipts = n("PREWRTWGHTS"),
                    .total = n("SPOTWGHTS"),
                    .receipts = n("WRTWGHTS"),
                    .total_change = n("SPOTCHANGE"),
                    .receipts_change = n("WRTCHANGE"),
                    .previous_capacity = n("PREWHSTOCKS"),
                    .capacity = n("WHSTOCKS"),
                    .capacity_change = n("WHSTOCKCHANGE"),
                    .published_ms = published,
                    .observed_ms = observed});
        }
    }
    // 黄金原报说明库存就是已注册成标准仓单的数量，只公布汇总。
    if (root.contains("o_cursorau") && !root.at("o_cursorau").is_null()) {
        if (!root.at("o_cursorau").is_array())
            throw std::invalid_argument("expected exchange rows");
        for (const auto& row : root.at("o_cursorau"))
            result.push_back(
                {.exchange = "SHFE",
                    .product = "au",
                    .day = day,
                    .warehouse = std::string("总计"),
                    .unit = shanghai_unit(row.at("WGHTUNIT")),
                    .previous_total = number(row.at("PREWRTWGHTS")),
                    .previous_receipts = number(row.at("PREWRTWGHTS")),
                    .total = number(row.at("WRTWGHTS")),
                    .receipts = number(row.at("WRTWGHTS")),
                    .total_change = number(row.at("WRTCHANGE")),
                    .receipts_change = number(row.at("WRTCHANGE")),
                    .published_ms = published,
                    .observed_ms = observed});
    }
    return result;
}

std::vector<MemberPosition>
shanghai_members(const std::string& exchange, const std::string& day, const Json& root)
{
    const auto published = shanghai_published(root);
    const auto observed = observation_time();
    const std::pair<Cell MemberPosition::*, Cell MemberPosition::*> metrics[] = {
        {&MemberPosition::volume, &MemberPosition::volume_change},
        {&MemberPosition::long_position, &MemberPosition::long_change},
        {&MemberPosition::short_position, &MemberPosition::short_change}
    };
    std::map<std::pair<std::string, std::string>, MemberPosition> members;
    for (const auto& row : root.at("o_cursor")) {
        const auto code = text(row.at("INSTRUMENTID"));
        const auto contract = contract_of(exchange, code, day);
        const auto rank = values::numeric(row.at("RANK"));
        // 早年的 *actv 只合计活跃月份，合约行里的 -1、0 是会员类型汇总，都不是会员记录。
        if ((!contract && !code.ends_with("all")) || rank == "999"
            || (contract && (rank == "-1" || rank == "0")))
            continue;
        const auto product = contract ? product_of(*contract) : code.substr(0, code.size() - 3);
        // 上期所排名文件同时列出能源中心品种。
        if (exchange == "SHFE" && ine_products.contains(product))
            continue;
        for (std::size_t i = 0; i < std::size(metrics); ++i) {
            const auto suffix = std::to_string(i + 1);
            const auto name = text(row.at("PARTICIPANTABBR" + suffix));
            if (name.empty())
                continue;
            auto [at, inserted] = members.try_emplace(
                std::pair{code, name},
                MemberPosition{
                    .exchange = exchange,
                    .product = product,
                    .contract = contract ? Cell{*contract} : Cell{},
                    .day = day,
                    .member = name,
                    .published_ms = published,
                    .observed_ms = observed
                });
            auto& member = at->second;
            const auto [quantity, change] = metrics[i];
            if (member.*quantity != Cell{})
                throw std::invalid_argument("duplicate member in exchange ranking");
            member.*quantity = number(row.at("CJ" + suffix));
            member.*change = number(row.at("CJ" + suffix + "_CHG"));
        }
    }
    std::vector<MemberPosition> result;
    for (auto& [key, member] : members)
        result.push_back(std::move(member));
    return result;
}

} // namespace

std::vector<WarehouseReceipt> shfe_html_receipts(const std::string& day, const std::string& raw)
{
    auto document = shfe_document(raw);
    auto* stock = lxb_dom_node_by_id(
        lxb_dom_interface_node(document.get()),
        reinterpret_cast<const lxb_char_t*>("stock"),
        5);
    const auto tables = children(stock, LXB_TAG_TABLE);
    if (tables.size() < 2 || !html::text(tables.front()).starts_with(day + " "))
        throw std::invalid_argument("missing or misdated SHFE receipt report");
    const auto observed = observation_time();
    std::vector<WarehouseReceipt> result;
    for (std::size_t index = 1; index < tables.size(); ++index) {
        auto* table = tables[index];
        const auto rows = children(html::first_child_tag(table, LXB_TAG_TBODY), LXB_TAG_TR);
        const auto title = children(rows.at(0), LXB_TAG_TD);
        const auto [product, type] = shanghai_product(html::text(title.at(0)));
        if (ine_products.contains(product))
            continue;
        const std::string_view prefix = "单位：";
        const auto units = html::text(title.at(1));
        if (!units.starts_with(prefix) || units.size() == prefix.size())
            throw std::invalid_argument("missing SHFE receipt unit");
        const auto unit = units.substr(prefix.size());
        auto* head = html::first_child_tag(table, LXB_TAG_THEAD);
        const auto headings = children(html::first_child_tag(head, LXB_TAG_TR), LXB_TAG_TH);
        const auto warehouse_column = html::text(headings.at(0)) == "地区" ? 1U : 0U;
        const auto area_column = 1 - warehouse_column;
        const auto warehouse_label = html::text(headings.at(warehouse_column));
        if (headings.size() != 4 || html::text(headings[area_column]) != "地区"
            || (warehouse_label != "仓库" && warehouse_label != "厂库")
            || html::text(headings[2]) != "期货" || html::text(headings[3]) != "增减")
            throw std::invalid_argument("unexpected SHFE receipt headings");
        FirstColumn leading;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            auto* row = rows[i];
            auto cells = children(row, LXB_TAG_TD);
            leading.expand(cells);
            if (attribute(row, "class").find("isTotal") != std::string_view::npos)
                continue;
            const bool merged = cells.size() == 3 && attribute(cells[0], "colspan") == "2";
            if (!merged && cells.size() != 4)
                throw std::invalid_argument("wrong SHFE receipt columns");
            const auto area = merged ? std::string{} : html::text(cells[area_column]);
            const auto volume = number(html::text(cells[cells.size() - 2]));
            const auto change = number(html::text(cells.back()));
            result.push_back(
                {.exchange = "SHFE",
                    .product = product,
                    .day = day,
                    .warehouse = html::text(cells[merged ? 0 : warehouse_column]),
                    .warehouse_type = type,
                    .area = area.empty() ? Cell{} : Cell{area},
                    .unit = unit,
                    .previous_volume = previous_receipts(volume, change),
                    .volume = volume,
                    .volume_change = change,
                    .observed_ms = observed});
        }
        if (leading.remaining)
            throw std::invalid_argument("incomplete SHFE receipt rowspan");
    }
    return result;
}

std::vector<Inventory> shfe_html_inventory(const std::string& day, const std::string& raw)
{
    auto document = shfe_document(raw);
    auto* root = lxb_dom_interface_node(document.get());
    const auto observed = observation_time();
    std::vector<Inventory> result;
    if (auto* stock = lxb_dom_node_by_id(root, reinterpret_cast<const lxb_char_t*>("stock"), 5)) {
        const auto tables = children(stock, LXB_TAG_TABLE);
        if (tables.empty() || !html::text(tables.front()).starts_with(day + " "))
            throw std::invalid_argument("missing or misdated SHFE inventory report");
        for (auto* table : std::span(tables).subspan(1)) {
            auto* head = html::first_child_tag(table, LXB_TAG_THEAD);
            if (!head && html::text(table).starts_with("注"))
                continue;
            const auto heads = children(head, LXB_TAG_TR);
            const auto body = children(html::first_child_tag(table, LXB_TAG_TBODY), LXB_TAG_TR);
            inventory_section(
                result,
                day,
                observed,
                children(body.at(0), LXB_TAG_TD),
                children(heads.at(0), LXB_TAG_TH),
                heads.size() > 1 ? children(heads[1], LXB_TAG_TH) : Nodes{},
                std::span(body).subspan(1),
                false);
        }
    } else {
        lxb_dom_node_t* table = nullptr;
        lxb_dom_node_simple_walk(
            root,
            [](lxb_dom_node_t* node, void* found) {
                if (!html::is_tag(node, LXB_TAG_TABLE))
                    return LEXBOR_ACTION_OK;
                *static_cast<lxb_dom_node_t**>(found) = node;
                return LEXBOR_ACTION_STOP;
            },
            &table);
        const auto rows = children(html::first_child_tag(table, LXB_TAG_TBODY), LXB_TAG_TR);
        const auto date = "日期：" + day.substr(0, 4) + "年"
            + std::to_string(std::stoi(day.substr(5, 2))) + "月"
            + std::to_string(std::stoi(day.substr(8))) + "日";
        if (rows.size() < 3 || html::text(rows[0]).find("库存周报") == std::string::npos
            || html::text(children(rows[1], LXB_TAG_TD).at(0)) != date)
            throw std::invalid_argument("missing or misdated SHFE inventory archive");
        for (std::size_t i = 2; i < rows.size();) {
            const auto start = i++;
            if (html::text(rows[start]).find("单位：") == std::string::npos)
                continue;
            while (i < rows.size() && html::text(rows[i]).find("单位：") == std::string::npos)
                ++i;
            const auto title = children(rows[start], LXB_TAG_TD);
            const auto head = children(rows.at(start + 1), LXB_TAG_TD);
            const bool gold = html::text(head.at(0)) == "上周库存";
            const auto first = start + (gold ? 2 : 3);
            if (i <= first)
                throw std::invalid_argument("missing SHFE archive inventory rows");
            inventory_section(
                result,
                day,
                observed,
                title,
                head,
                gold ? Nodes{} : children(rows[start + 2], LXB_TAG_TD),
                std::span(rows).subspan(first, i - first),
                true);
        }
    }
    if (result.empty())
        throw std::invalid_argument("empty SHFE inventory report");
    return result;
}

Parsed parse_shanghai(
    const std::string& exchange,
    const std::string& day,
    const std::string& table,
    const std::string& raw)
{
    const auto root = values::parse(raw);
    // Until 2024-03-20 the status is the number 0, then the text "0000".
    const auto& code = root.at("o_code");
    const auto status = code.is_string() ? text(code) : values::numeric(code);
    if (status != "0000" && status != "0")
        throw std::invalid_argument("refused exchange file");
    Parsed result;
    std::set<std::string> seen;
    const auto& rows = root.at(table == "daily" ? "o_curinstrument" : "o_cursor");
    if (!rows.is_array())
        throw std::invalid_argument("expected exchange rows");
    const auto* date_field =
        table == "warehouse_receipts" || table == "inventory" ? "o_tradingday" : "report_date";
    // 早期排名和上期所 2015-05-25 前的结算原报只在文件路径标日期。
    const bool path_dated = table == "member_positions"
        || (exchange == "SHFE" && table == "settlement" && day < "2015-05-25");
    if ((!path_dated || root.contains(date_field)) && text(root.at(date_field)) != compact(day))
        throw std::invalid_argument("wrong exchange report date");
    if (table == "warehouse_receipts") {
        result.warehouse_receipts = shanghai_receipts(exchange, day, root);
        return result;
    }
    if (table == "inventory") {
        result.inventory = shanghai_inventory(day, root);
        return result;
    }
    if (table == "member_positions") {
        result.member_positions = shanghai_members(exchange, day, root);
        return result;
    }
    for (const auto& row : rows) {
        std::string code_text;
        if (table == "daily") {
            auto product = row.contains("PRODUCTGROUPID") ? text(row.at("PRODUCTGROUPID"))
                                                          : text(row.at("PRODUCTID"));
            if (!row.contains("PRODUCTGROUPID") && product.ends_with("_f"))
                product.resize(product.size() - 2);
            code_text = product + text(row.at("DELIVERYMONTH"));
        } else
            code_text = text(row.at("INSTRUMENTID"));
        const auto contract = contract_of(exchange, code_text, day);
        if (!contract || ine_products.contains(product_of(*contract)) != (exchange == "INE"))
            continue;
        unique_contract(seen, *contract);
        const auto n = [&](const char* key, int shift = 0) {
            return number(row.at(key), shift);
        };
        if (table == "daily")
            result.daily.push_back(daily_row(
                exchange,
                *contract,
                day,
                n("OPENPRICE"),
                n("HIGHESTPRICE"),
                n("LOWESTPRICE"),
                n("CLOSEPRICE"),
                n("SETTLEMENTPRICE"),
                n("PRESETTLEMENTPRICE"),
                n("VOLUME"),
                n("OPENINTEREST")));
        else {
            // Published since 2024-07-22; missing before, never guessed.
            const auto today = [&](const char* key, int shift = 0) {
                return row.contains(key) ? n(key, shift) : Cell{};
            };
            result.settlement.push_back(
                {.exchange = exchange,
                    .product = product_of(*contract),
                    .contract = *contract,
                    .day = day,
                    .long_margin_rate = n("SPECLONGMARGINRATIO"),
                    .short_margin_rate = n("SPECSHORTMARGINRATIO"),
                    .fee_rate = n("TRADEFEERATIO", -3),
                    .fee_per_lot = n("TRADEFEEUNIT"),
                    .close_today_fee_rate = today("TTRADEFEERATIO", -3),
                    .close_today_fee_per_lot = today("TTRADEFEEUNIT")});
        }
    }
    return result;
}

} // namespace asterion::warehouse::acquisition::detail
