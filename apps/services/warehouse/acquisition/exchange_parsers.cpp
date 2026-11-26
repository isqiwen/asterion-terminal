#include "exchange_parsers.hpp"
#include <asterion/base/decimal.hpp>
#include <iconv.h>
#include <tinyxml2.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <set>

namespace asterion::warehouse::acquisition::detail {
namespace {

using values::Json;
using warehouse::Cell;
using Parsed = Exchange::Parsed;

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

} // namespace

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

Cell number(const Json& value, int shift)
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

void unique_contract(std::set<std::string>& seen, const std::string& contract)
{
    if (!seen.insert(contract).second)
        throw std::invalid_argument("duplicate exchange contract");
}

std::vector<PriceTick>
parse_czce_price_ticks(const std::string& day, const std::string& raw, const std::string& url)
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
        unique_contract(seen, identity);
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

Parsed parse_text_rows(
    const std::string& exchange,
    const std::string& day,
    const std::string& table,
    const std::string& raw,
    bool archive)
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
            unique_contract(seen, *contract);
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
        unique_contract(seen, *contract);
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

Parsed parse_portal(
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
        unique_contract(seen, *contract);
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

} // namespace asterion::warehouse::acquisition::detail
