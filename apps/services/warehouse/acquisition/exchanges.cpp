#include "exchanges.hpp"
#include "archive.hpp"
#include "values.hpp"
#include <asterion/base/decimal.hpp>
#include <httplib.h>
#include <iconv.h>
#include <tinyxml2.h>
#include <algorithm>
#include <cctype>
#include <set>

namespace asterion::warehouse::acquisition {
namespace {

using values::Json;
using warehouse::Cell;
using warehouse::Row;

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
        iconv_t value = iconv_open("UTF-8", "GB18030");

        ~Converter()
        {
            if (value != reinterpret_cast<iconv_t>(-1))
                iconv_close(value);
        }
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

Row daily_row(
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
    const auto begin = contract.find('/') + 1;
    return {
        exchange,
        contract.substr(begin, contract.rfind('/') - begin),
        contract,
        day,
        open,
        high,
        low,
        close,
        settle,
        Cell{},
        previous,
        volume,
        Cell{},
        interest,
        Cell{},
        Cell{}
    };
}

void unique(std::set<std::string>& seen, const std::string& contract)
{
    if (!seen.insert(contract).second)
        throw std::invalid_argument("duplicate exchange contract");
}

Rows czce_price_ticks(const std::string& day, const std::string& raw, const std::string& url)
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
    Rows result;
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
        result.push_back({"CZCE", product, identity, day, until, tick.str(), url});
    }
    if (result.empty())
        throw std::invalid_argument("CZCE reference contains no futures contracts");
    return result;
}

Rows shanghai(
    const std::string& exchange,
    const std::string& day,
    const std::string& table,
    const std::string& raw)
{
    const auto root = values::parse(raw);
    if (text(root.at("report_date")) != compact(day) || text(root.at("o_code")) != "0000")
        throw std::invalid_argument("wrong date or refused exchange file");
    Rows result;
    std::set<std::string> seen;
    const auto& rows = root.at(table == "daily" ? "o_curinstrument" : "o_cursor");
    if (!rows.is_array())
        throw std::invalid_argument("expected exchange rows");
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
            result.push_back(daily_row(
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
            const auto begin = contract->find('/') + 1;
            result.push_back(
                {exchange,
                    contract->substr(begin, contract->rfind('/') - begin),
                    *contract,
                    day,
                    n("SPECLONGMARGINRATIO"),
                    n("SPECSHORTMARGINRATIO"),
                    n("TRADEFEERATIO", -3),
                    n("TRADEFEEUNIT"),
                    n("TTRADEFEERATIO", -3),
                    n("TTRADEFEEUNIT")});
        }
    }
    return result;
}

Rows text_rows(
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
        Rows result;
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
            result.push_back(daily_row(
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
    Rows result;
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
            result.push_back(daily_row(
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
            const bool ratio = value("手续费收取方式") == "比例值";
            if (!ratio && value("手续费收取方式") != "绝对值")
                throw std::invalid_argument("unknown fee basis");
            const auto begin = contract->find('/') + 1;
            const auto margin = n("交易保证金率(%)", -2);
            const Cell zero{std::string("0")};
            result.push_back(
                {exchange,
                    contract->substr(begin, contract->rfind('/') - begin),
                    *contract,
                    day,
                    margin,
                    margin,
                    ratio ? n("交易手续费", -4) : zero,
                    ratio ? zero : n("交易手续费"),
                    ratio ? n("日内平今仓交易手续费", -4) : zero,
                    ratio ? zero : n("日内平今仓交易手续费")});
        }
    }
    return result;
}

Rows portal(const std::string& exchange, const std::string& day, const std::string& raw)
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
    Rows result;
    std::set<std::string> seen;
    for (const auto& row : rows) {
        const auto label = text(row.at("variety"));
        if (label.find("小计") != std::string::npos || label.find("总计") != std::string::npos)
            continue;
        const auto code = exchange == "GFEX"
            ? text(row.at("varietyOrder")) + text(row.at("delivMonth"))
            : text(row.at("contractId"));
        const auto contract = contract_of(exchange, code, day);
        if (!contract)
            continue;
        unique(seen, *contract);
        const auto n = [&](const char* field) {
            return number(row.at(field));
        };
        result.push_back(daily_row(
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

Exchange::Exchange(std::string exchange, std::stop_token stop, Fetch fetch)
    : exchange_(std::move(exchange)),
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
            || (table == "settlement"
                && (exchange == "SHFE" || exchange == "INE" || exchange == "CZCE")));
}

Rows Exchange::daily(const std::string& exchange, const std::string& day)
{
    return read("daily", exchange, day);
}

Rows Exchange::settlement(const std::string& exchange, const std::string& day)
{
    return read("settlement", exchange, day);
}

Rows Exchange::price_ticks(const std::string& exchange, const std::string& day)
{
    return read("price_ticks", exchange, day);
}

Rows Exchange::read(const std::string& table, const std::string& exchange, const std::string& day)
{
    day_of(day);
    if (!supports(table, exchange))
        throw SourceError("unsupported", "exchange source does not provide the requested table");
    check_stop(stop_);
    const bool archived = exchange == "CZCE" && table == "daily" && day <= "2010-08-24";
    if (archived && archive_year_ == day.substr(0, 4)) {
        try {
            return archive_.empty() ? Rows{} : text_rows(exchange, day, table, archive_, true);
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
        request.path = "/data/tradedata/future/dailydata/"
            + std::string(table == "daily" ? "kx" : "js") + date + ".dat";
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
        request.path = "/u/interfacesWebTiDayQuotes/loadList";
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
            return czce_price_ticks(day, reply.body, request.origin + request.path);
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
        return portal(exchange, day, reply.body);
    } catch (const std::exception& error) {
        // Public files carry no credentials; name the broken field or invariant
        // so an upstream format change can be diagnosed without saving raw replies.
        throw SourceError("invalid_data", "invalid exchange " + table + " file: " + error.what());
    }
}

} // namespace asterion::warehouse::acquisition
