#include "exchange_parsers.hpp"
#include <asterion/base/decimal.hpp>
#include <chrono>
#include <map>
#include <regex>
#include <set>

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

std::pair<std::string, Cell> shanghai_product(const Json& row)
{
    // Early reports identify products by their Chinese names, without VARID.
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
        {"丁二烯橡胶", "br"}
    };
    auto name = shanghai_label(row.at("VARNAME"));
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
    return {row.contains("VARID") ? text(row.at("VARID")) : products.at(name), warehouse_type};
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
        Cell previous;
        if (volume != Cell{} && change != Cell{})
            previous = Decimal::parse(std::get<std::string>(volume))
                - Decimal::parse(std::get<std::string>(change));
        const auto area = shanghai_label(row.at("REGNAME"));
        result.push_back(
            {.exchange = exchange,
                .product = product,
                .day = day,
                .warehouse = shanghai_label(row.at("WHABBRNAME")),
                .warehouse_type = warehouse_type,
                .area = area.empty() ? Cell{} : Cell{area},
                .unit = shanghai_unit(row.at("WGHTUNIT")),
                .previous_volume = previous,
                .volume = volume,
                .volume_change = change,
                .published_ms = published,
                .observed_ms = observed});
    }
    return result;
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
    // 早年的排名文件没有报告日，日期只在请求路径里。
    if ((table != "member_positions" || root.contains(date_field))
        && text(root.at(date_field)) != compact(day))
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
        if (!contract)
            continue; // Options and summary rows are not futures contracts.
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
