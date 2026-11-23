#include "exchanges.hpp"
#include "archive.hpp"
#include "values.hpp"
#include <asterion/base/decimal.hpp>
#include <httplib.h>
#include <iconv.h>
#include <tinyxml2.h>
#include <algorithm>
#include <cctype>
#include <regex>
#include <set>

namespace asterion::warehouse::acquisition {
namespace {

using values::Json;
using warehouse::Cell;
using Parsed = Exchange::Parsed;
const std::set<std::string> ine_products{"bc", "ec", "lu", "nr", "sc"};

std::string trim(std::string text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos
        ? std::string{}
        : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string compact(std::string day)
{
    std::erase(day, '-');
    return day;
}

std::string text(const Json& value)
{
    return trim(value.get<std::string>());
}

Cell number(const Json& value, int shift = 0)
{
    if (value.is_null())
        return {};
    if (!value.is_string())
        return values::number(value, shift);
    auto raw = text(value);
    if (raw.empty() || raw == "-" || raw == "--")
        return {};
    std::erase(raw, ',');
    return values::number(values::parse(raw), shift);
}

std::string utf8(const std::string& raw)
{
    struct Converter
    {
        ~Converter()
        {
            if (value != reinterpret_cast<iconv_t>(-1))
                iconv_close(value);
        }

        iconv_t value = iconv_open("UTF-8", "GB18030");
    } converter;

    if (converter.value == reinterpret_cast<iconv_t>(-1))
        throw std::runtime_error("GB18030 converter is unavailable");
    std::string result(raw.size() * 4, '\0');
    char* input = const_cast<char*>(raw.data());
    char* output = result.data();
    std::size_t remaining = raw.size(), space = result.size();
    if (iconv(converter.value, &input, &remaining, &output, &space) == static_cast<std::size_t>(-1)
        || remaining)
        throw std::invalid_argument("invalid exchange file encoding");
    result.resize(result.size() - space);
    return result;
}

// CSV (CFFEX) and pipe-delimited files (CZCE), including quoted fields.
std::vector<std::vector<std::string>> cells(const std::string& raw, char separator)
{
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cell;
    bool quoted = false;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '"') {
            if (quoted && i + 1 < raw.size() && raw[i + 1] == '"') {
                cell += '"';
                ++i;
            } else
                quoted = !quoted;
        } else if (!quoted && (c == separator || c == '\n')) {
            row.push_back(trim(std::move(cell)));
            cell.clear();
            if (c == '\n') {
                rows.push_back(std::move(row));
                row.clear();
            }
        } else
            cell += c;
    }
    if (quoted)
        throw std::invalid_argument("unterminated CSV cell");
    if (!cell.empty() || !row.empty()) {
        row.push_back(trim(std::move(cell)));
        rows.push_back(std::move(row));
    }
    return rows;
}

DailyBar daily_row(
    const std::string& exchange,
    const std::string& contract,
    const std::string& day,
    Cell open,
    Cell high,
    Cell low,
    Cell close,
    Cell settle,
    Cell previous,
    Cell volume,
    Cell interest)
{
    const auto price = [](Cell value) {
        return value == Cell{std::string("0")} ? Cell{} : value;
    };
    open = price(std::move(open));
    high = price(std::move(high));
    low = price(std::move(low));
    close = open == Cell{} && high == Cell{} && low == Cell{} ? Cell{} : price(std::move(close));
    return {
        .exchange = exchange,
        .product = product_of(contract),
        .contract = contract,
        .day = day,
        .open = open,
        .high = high,
        .low = low,
        .close = close,
        .settlement = settle,
        .previous_settlement = previous,
        .volume = volume,
        .open_interest = interest
    };
}

void unique(std::set<std::string>& seen, const std::string& contract)
{
    if (!seen.insert(contract).second)
        throw std::invalid_argument("duplicate exchange contract");
}

std::vector<PriceTick>
czce_price_ticks(const std::string& day, const std::string& raw, const std::string& url)
{
    tinyxml2::XMLDocument document;
    if (document.Parse(raw.data(), raw.size()) != tinyxml2::XML_SUCCESS)
        throw std::invalid_argument("invalid CZCE reference XML");
    const auto* root = document.RootElement();
    if (!root || std::string_view(root->Name()) != "Contracts" || root->NextSiblingElement())
        throw std::invalid_argument("missing CZCE reference contracts");
    const auto observation = day_of(day);
    const auto until = day_text(observation + std::chrono::days(1));
    constexpr std::string_view unit = "元/吨";
    std::vector<PriceTick> result;
    std::set<std::string> seen;
    for (const auto* row = root->FirstChildElement(); row; row = row->NextSiblingElement()) {
        if (std::string_view(row->Name()) != "Contract")
            throw std::invalid_argument("unexpected CZCE reference element");
        const auto field = [&](const char* name) {
            const auto* element = row->FirstChildElement(name);
            if (!element || !element->GetText() || element->NextSiblingElement(name)
                || element->FirstChildElement())
                throw std::invalid_argument("invalid CZCE reference field: " + std::string(name));
            return trim(element->GetText());
        };
        if (field("PrdTp") != "期货")
            continue;
        if (field("ExchCd") != "XZCE" || field("TrdCcyCd") != "CNY" || field("MsrmntUnt") != "吨")
            throw std::invalid_argument("unexpected CZCE reference exchange or price unit");
        auto product = field("PrdCd");
        std::transform(product.begin(), product.end(), product.begin(), [](unsigned char c) {
            return std::tolower(c);
        });
        const auto identity = "CZCE/" + product + "/" + field("YrCd") + "-" + field("MnthCd");
        require_contract(identity);
        if (contract_of("CZCE", field("CtrCd"), day) != identity)
            throw std::invalid_argument("inconsistent CZCE reference contract");
        // The XML has no report date; the fixed URL dates this observation only.
        if (day_of(field("FrstTrdDt")) > observation || day_of(field("LstTrdDt")) < observation)
            throw std::invalid_argument("CZCE reference contract not listed on requested day");
        const auto quoted = field("TckSz");
        if (!quoted.ends_with(unit))
            throw std::invalid_argument("invalid CZCE reference price tick or unit");
        const auto tick = Decimal::parse(quoted.substr(0, quoted.size() - unit.size()));
        if (tick <= Decimal{})
            throw std::invalid_argument("CZCE reference price tick must be positive");
        unique(seen, identity);
        result.push_back(
            {.exchange = "CZCE",
                .product = product,
                .contract = identity,
                .effective_from = day,
                .effective_until = until,
                .price_tick = tick,
                .source_url = url});
    }
    if (result.empty())
        throw std::invalid_argument("CZCE reference contains no futures contracts");
    return result;
}

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
        if (!root.contains(cursor))
            continue;
        for (const auto& row : root.at(cursor)) {
            if (text(row.at("ROWSTATUS")) != "0")
                continue;
            const auto [product, warehouse_type] = shanghai_product(row);
            if (product == "au" || ine_products.contains(product))
                continue;
            const auto area = shanghai_label(row.at("REGNAME"));
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
                    .grade = row.contains("GRDNAME") ? Cell{shanghai_label(row.at("GRDNAME"))}
                        : row.contains("OILNAME")    ? Cell{shanghai_label(row.at("OILNAME"))}
                                                     : Cell{},
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

Parsed shanghai(
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
        std::string code;
        if (table == "daily") {
            auto product = row.contains("PRODUCTGROUPID") ? text(row.at("PRODUCTGROUPID"))
                                                          : text(row.at("PRODUCTID"));
            if (!row.contains("PRODUCTGROUPID") && product.ends_with("_f"))
                product.resize(product.size() - 2);
            code = product + text(row.at("DELIVERYMONTH"));
        } else
            code = text(row.at("INSTRUMENTID"));
        const auto contract = contract_of(exchange, code, day);
        if (!contract)
            continue; // Options and summary rows are not futures contracts.
        unique(seen, *contract);
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

Parsed text_rows(
    const std::string& exchange,
    const std::string& day,
    const std::string& table,
    const std::string& raw,
    bool archive = false)
{
    const auto decoded = !archive && (exchange == "CFFEX" || day < "2017-12-28") ? utf8(raw) : raw;
    const bool old_daily = exchange == "CZCE" && table == "daily" && !archive && day < "2015-11-12";
    const auto rows = cells(decoded, exchange == "CFFEX" || old_daily ? ',' : '|');
    // CZCE dates its title. CFFEX's CSV is dated only by its fixed request path.
    if (exchange == "CZCE" && !archive
        && (rows.empty() || rows[0].empty() || rows[0][0].find(day) == std::string::npos))
        throw std::invalid_argument("wrong CZCE report date");
    if (old_daily) {
        Parsed result;
        std::set<std::string> seen;
        for (const auto& row : rows) {
            if (row.empty())
                continue;
            const auto contract = contract_of(exchange, row[0], day);
            if (!contract)
                continue;
            unique(seen, *contract);
            if (row.size() != 14)
                throw std::invalid_argument("invalid historical CZCE daily row");
            const auto n = [&](std::size_t at) {
                return number(Json(row.at(at)));
            };
            result.daily.push_back(daily_row(
                exchange,
                *contract,
                day,
                n(2),
                n(3),
                n(4),
                n(5),
                n(6),
                n(1),
                n(9),
                n(10)));
        }
        return result;
    }
    const auto header = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
        return !row.empty()
            && (row[0] == "合约代码" || row[0] == "品种月份" || row[0] == "品种代码"
                || row[0] == "交易日期");
    });
    if (header == rows.end())
        throw std::invalid_argument("missing exchange table header");
    std::map<std::string, std::size_t> columns;
    for (std::size_t i = 0; i < header->size(); ++i)
        if (!columns.emplace((*header)[i], i).second)
            throw std::invalid_argument("duplicate exchange table header");
    Parsed result;
    std::set<std::string> seen;
    for (auto item = std::next(header); item != rows.end(); ++item) {
        if (item->empty())
            continue;
        if (archive) {
            auto date = (*item)[0];
            std::erase(date, '/');
            std::erase(date, '-');
            if (date != compact(day))
                continue;
        }
        const auto contract = contract_of(exchange, item->at(archive ? 1 : 0), day);
        if (!contract)
            continue;
        unique(seen, *contract);
        const auto value = [&](const char* key) -> const std::string& {
            return item->at(columns.at(key));
        };
        const auto n = [&](const char* key, int shift = 0) {
            return number(Json(value(key)), shift);
        };
        if (table == "daily")
            result.daily.push_back(daily_row(
                exchange,
                *contract,
                day,
                n("今开盘"),
                n("最高价"),
                n("最低价"),
                n("今收盘"),
                n("今结算"),
                n(exchange == "CZCE" ? "昨结算" : "前结算"),
                n(exchange == "CZCE" ? "成交量(手)" : "成交量"),
                n(columns.contains("持仓量") ? "持仓量" : "空盘量")));
        else {
            // CZCE added the fee basis on 2022-11-07, all per lot; its first ratio came on 2023-07-20.
            const std::string basis =
                exchange == "CZCE" && day < "2022-11-07" ? "绝对值" : value("手续费收取方式");
            const bool ratio = basis == "比例值";
            if (!ratio && basis != "绝对值")
                throw std::invalid_argument("unknown fee basis");
            // Until 2019 CZCE gave buy and sell margins apart.
            const bool one_margin = columns.contains("交易保证金率(%)");
            const auto long_margin = n(one_margin ? "交易保证金率(%)" : "买交易保证金率(%)", -2);
            const auto short_margin = n(one_margin ? "交易保证金率(%)" : "卖交易保证金率(%)", -2);
            // Until 2020-07-31 "平今仓手续费"; before that a flag that halves the fee.
            Cell close_today;
            if (columns.contains("平今手续费减半")) {
                const auto& halved = value("平今手续费减半");
                if (halved != "Y" && halved != "N")
                    throw std::invalid_argument("unknown close-today fee flag");
                const auto fee = Decimal::parse(value("交易手续费"));
                close_today =
                    number(Json((halved == "Y" ? fee * Decimal::parse("0.5") : fee).str()));
            } else
                close_today = columns.contains("日内平今仓交易手续费") ? n("日内平今仓交易手续费")
                                                                       : n("平今仓手续费");
            const Cell zero{std::string("0")};
            result.settlement.push_back(
                {.exchange = exchange,
                    .product = product_of(*contract),
                    .contract = *contract,
                    .day = day,
                    .long_margin_rate = long_margin,
                    .short_margin_rate = short_margin,
                    .fee_rate = ratio ? n("交易手续费", -4) : zero,
                    .fee_per_lot = ratio ? zero : n("交易手续费"),
                    .close_today_fee_rate = ratio ? n("日内平今仓交易手续费", -4) : zero,
                    .close_today_fee_per_lot = ratio ? zero : close_today});
        }
    }
    return result;
}

Parsed portal(
    const std::string& exchange,
    const std::string& day,
    const std::string& table,
    const std::string& raw)
{
    const auto root = values::parse(raw);
    const auto& code = root.at("code");
    const auto status = code.is_string() ? text(code) : values::numeric(code);
    if (status != "0" && status != "200")
        throw std::invalid_argument("exchange portal refused request");
    if (exchange == "GFEX" && root.at("param").at("trade_date").at(0) != compact(day))
        throw std::invalid_argument("wrong portal date");
    const auto& rows = root.at("data");
    if (!rows.is_array())
        throw std::invalid_argument("expected portal rows");
    Parsed result;
    std::set<std::string> seen;
    for (const auto& row : rows) {
        const auto label = text(row.at("variety"));
        if (label.find("小计") != std::string::npos || label.find("总计") != std::string::npos)
            continue;
        const auto instrument = exchange == "GFEX" && table == "daily"
            ? text(row.at("varietyOrder")) + text(row.at("delivMonth"))
            : text(row.at("contractId"));
        const auto contract = contract_of(exchange, instrument, day);
        if (!contract)
            continue;
        unique(seen, *contract);
        const auto n = [&](const char* field, int shift = 0) {
            return number(row.at(field), shift);
        };
        if (table == "daily")
            result.daily.push_back(daily_row(
                exchange,
                *contract,
                day,
                n("open"),
                n("high"),
                n("low"),
                n("close"),
                n("clearPrice"),
                n("lastClear"),
                n("volumn"),
                n("openInterest")));
        else {
            const auto basis = text(row.at("style"));
            const bool ratio = basis == "比例值";
            if (!ratio && basis != "绝对值")
                throw std::invalid_argument("unknown fee basis");
            const auto open_fee = n("openFee"), today_fee = n("shortOffsetFee");
            if (!std::holds_alternative<std::string>(open_fee)
                || !std::holds_alternative<std::string>(today_fee)
                || !std::holds_alternative<std::string>(n("shortOpenFee"))
                || open_fee != n("offsetFee"))
                throw std::invalid_argument("invalid GFEX settlement fee");
            const Cell zero{std::string("0")};
            result.settlement.push_back(
                {.exchange = exchange,
                    .product = product_of(*contract),
                    .contract = *contract,
                    .day = day,
                    .long_margin_rate = n("specBuyRate"),
                    .short_margin_rate = n("specSellRate"),
                    .fee_rate = ratio ? n("openFee", -4) : zero,
                    .fee_per_lot = ratio ? zero : open_fee,
                    .close_today_fee_rate = ratio ? n("shortOffsetFee", -4) : zero,
                    .close_today_fee_per_lot = ratio ? zero : today_fee});
        }
    }
    return result;
}

} // namespace

HttpReply exchange_fetch(const HttpRequest& request, std::stop_token stop)
{
    check_stop(stop);
    httplib::Client client(request.origin);
    client.enable_server_certificate_verification(true);
    client.set_follow_location(false);
    client.set_connection_timeout(10);
    client.set_read_timeout(30);
    client.set_write_timeout(10);
    client.set_max_timeout(std::chrono::seconds(60));
    client.set_payload_max_length(values::response_limit);
    client.set_default_headers({{"User-Agent", "Mozilla/5.0"}});
    std::stop_callback cancel(stop, [&] {
        client.stop();
    });
    auto result = request.body.empty()
        ? client.Get(request.path)
        : client.Post(request.path, request.body, request.content_type);
    check_stop(stop);
    if (!result)
        throw SourceError("unavailable", "exchange file request failed");
    return {result->status, std::move(result->body)};
}

Exchange::Exchange(
    std::string exchange,
    std::shared_ptr<Budget> budget,
    std::stop_token stop,
    Fetch fetch)
    : exchange_(std::move(exchange)),
      budget_(std::move(budget)),
      stop_(stop),
      fetch_(std::move(fetch))
{
    require_exchange(exchange_);
    if (!fetch_)
        throw std::invalid_argument("missing exchange transport");
}

bool Exchange::supports(std::string_view table, std::string_view exchange) const
{
    return exchange == exchange_
        && (table == "daily" || (table == "price_ticks" && exchange == "CZCE")
            || (table == "inventory" && exchange == "SHFE")
            || ((table == "member_positions" || table == "warehouse_receipts")
                && (exchange == "SHFE" || exchange == "INE"))
            || (table == "settlement"
                && (exchange == "SHFE" || exchange == "INE" || exchange == "CZCE"
                    || exchange == "GFEX")));
}

std::vector<DailyBar> Exchange::daily(const std::string& exchange, const std::string& day)
{
    return read("daily", exchange, day).daily;
}

std::vector<Settlement> Exchange::settlement(const std::string& exchange, const std::string& day)
{
    return read("settlement", exchange, day).settlement;
}

std::vector<PriceTick> Exchange::price_ticks(const std::string& exchange, const std::string& day)
{
    return read("price_ticks", exchange, day).price_ticks;
}

std::vector<MemberPosition>
Exchange::member_positions(const std::string& exchange, const std::string& day)
{
    return read("member_positions", exchange, day).member_positions;
}

std::vector<WarehouseReceipt>
Exchange::warehouse_receipts(const std::string& exchange, const std::string& day)
{
    return read("warehouse_receipts", exchange, day).warehouse_receipts;
}

std::vector<Inventory> Exchange::inventory(const std::string& exchange, const std::string& day)
{
    return read("inventory", exchange, day).inventory;
}

Exchange::Parsed
Exchange::read(const std::string& table, const std::string& exchange, const std::string& day)
{
    day_of(day);
    if (!supports(table, exchange))
        throw SourceError("unsupported", "exchange source does not provide the requested table");
    check_stop(stop_);
    const bool archived = exchange == "CZCE" && table == "daily" && day <= "2010-08-24";
    if (archived && archive_year_ == day.substr(0, 4)) {
        try {
            return archive_.empty() ? Parsed{} : text_rows(exchange, day, table, archive_, true);
        } catch (const std::exception& error) {
            throw SourceError(
                "invalid_data",
                "invalid exchange daily archive: " + std::string(error.what()));
        }
    }
    const auto date = compact(day);
    HttpRequest request;
    if (exchange == "SHFE" || exchange == "INE") {
        request.origin = exchange == "SHFE" ? "https://www.shfe.com.cn" : "https://www.ine.cn";
        request.path = table == "inventory"
            ? "/data/tradedata/future/weeklydata/" + date + "weeklystock.dat"
            : table == "warehouse_receipts"
            ? "/data/tradedata/future/dailydata/" + date + "dailystock.dat"
            : "/data/tradedata/future/dailydata/"
                + std::string(
                    table == "daily"                  ? "kx"
                        : table == "member_positions" ? "pm"
                                                      : "js")
                + date + ".dat";
    } else if (exchange == "CZCE") {
        request.origin = "https://www.czce.com.cn";
        if (archived)
            request.path = "/cn/exchange/datahistory" + day.substr(0, 4) + ".zip";
        else if (table == "price_ticks")
            request.path = "/cn/DFSStaticFiles/Future/" + day.substr(0, 4) + "/" + date
                + "/FutureDataReferenceData.xml";
        else if (table == "daily" && day < "2015-11-12")
            request.path = "/cn/exchange/" + day.substr(0, 4) + "/datadaily/" + date + ".txt";
        else
            request.path = "/cn/DFSStaticFiles/Future/" + day.substr(0, 4) + "/" + date
                + (table == "daily" ? "/FutureDataDaily.txt" : "/FutureDataClearParams.txt");
    } else if (exchange == "CFFEX") {
        request.origin = "http://www.cffex.com.cn";
        request.path =
            "/sj/hqsj/rtj/" + date.substr(0, 6) + "/" + date.substr(6) + "/" + date + "_1.csv";
    } else if (exchange == "GFEX") {
        request.origin = "http://www.gfex.com.cn";
        request.path = table == "daily" ? "/u/interfacesWebTiDayQuotes/loadList"
                                        : "/u/interfacesWebTiFutAndOptSettle/loadList";
        request.body = "trade_date=" + date + "&trade_type=0";
        request.content_type = "application/x-www-form-urlencoded";
    } else {
        request.origin = "http://www.dce.com.cn";
        request.path = "/dcereport/publicweb/dailystat/dayQuotes";
        request.body =
            Json{
                {"contractId", ""},
                {"lang", "zh"},
                {"optionSeries", ""},
                {"statisticsType", "0"},
                {"tradeDate", date},
                {"tradeType", "1"},
                {"varietyId", "all"}
            }
                .dump();
        request.content_type = "application/json";
    }
    budget_->acquire(stop_);
    ++requests_;
    HttpReply reply;
    try {
        reply = fetch_(request, stop_);
    } catch (const SourceError&) {
        throw;
    } catch (...) {
        throw SourceError("unavailable", "exchange transport failed");
    }
    check_stop(stop_);
    if (reply.status == 404) {
        if (archived) {
            archive_year_ = day.substr(0, 4);
            archive_.clear();
        }
        return {};
    } // No publication is not proof of a closed day.
    if (reply.status != 200)
        throw SourceError(
            "unavailable",
            "exchange file refused (HTTP " + std::to_string(reply.status) + ")");
    try {
        if (reply.body.size() > values::response_limit)
            throw std::invalid_argument("exchange file too large");
        if (table == "price_ticks")
            return {
                .price_ticks = czce_price_ticks(day, reply.body, request.origin + request.path)
            };
        if (exchange == "SHFE" || exchange == "INE")
            return shanghai(exchange, day, table, reply.body);
        if (archived) {
            auto decoded =
                utf8(archive_member(reply.body, "datahistory" + day.substr(0, 4) + ".txt"));
            auto result = text_rows(exchange, day, table, decoded, true);
            archive_year_ = day.substr(0, 4);
            archive_ = std::move(decoded);
            return result;
        }
        if (exchange == "CZCE" || exchange == "CFFEX")
            return text_rows(exchange, day, table, reply.body);
        return portal(exchange, day, table, reply.body);
    } catch (const std::exception& error) {
        // Public files carry no credentials; name the broken field or invariant
        // so an upstream format change can be diagnosed without saving raw replies.
        throw SourceError("invalid_data", "invalid exchange " + table + " file: " + error.what());
    }
}

} // namespace asterion::warehouse::acquisition
