#include "exchanges.hpp"
#include "archive.hpp"
#include "exchange_parsers.hpp"
#include "values.hpp"
#include <httplib.h>
#include <map>
#include <set>

namespace asterion::warehouse::acquisition {
namespace {

using values::Json;
using warehouse::Cell;

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
    auto result = request.method == "GET"
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
            || (table == "member_positions" && exchange == "GFEX")
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

HttpReply Exchange::fetch(const HttpRequest& request)
{
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
    if (reply.status != 200 && reply.status != 404)
        throw SourceError(
            "unavailable",
            "exchange file refused (HTTP " + std::to_string(reply.status) + ")");
    if (reply.body.size() > values::response_limit)
        throw SourceError("invalid_data", "exchange file too large");
    return reply;
}

std::vector<MemberPosition> Exchange::gfex_members(const std::string& day)
{
    try {
        const auto ask = [&](const std::string& path, const Json& params) {
            HttpRequest request{
                "http://www.gfex.com.cn",
                "/u/" + path,
                {},
                "application/x-www-form-urlencoded",
                "POST"
            };
            for (const auto& [key, value] : params.items()) {
                if (!request.body.empty())
                    request.body += '&';
                request.body += key + "=" + detail::text(value);
            }
            const auto reply = fetch(request);
            if (reply.status != 200)
                throw SourceError("unavailable", "GFEX member ranking endpoint is unavailable");
            auto root = values::parse(reply.body);
            if (root.at("code") != "0" || !root.at("data").is_array())
                throw std::invalid_argument("GFEX member ranking request was refused");
            for (const auto& [key, value] : params.items())
                if (root.at("param").at(key).at(0) != value)
                    throw std::invalid_argument("wrong GFEX member ranking parameters");
            return std::move(root.at("data"));
        };
        const std::pair<Cell MemberPosition::*, Cell MemberPosition::*> metrics[] = {
            {&MemberPosition::volume, &MemberPosition::volume_change},
            {&MemberPosition::long_position, &MemberPosition::long_change},
            {&MemberPosition::short_position, &MemberPosition::short_change}
        };
        std::vector<MemberPosition> result;
        std::set<std::string> seen;
        for (const auto& variety : ask("interfacesWebVariety/loadList", Json::object())) {
            const auto product = detail::text(variety.at("varietyId"));
            if (product.empty()
                || product.find_first_not_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos)
                throw std::invalid_argument("invalid GFEX variety");
            for (const auto& item :
                ask("interfacesWebTiMemberDealPosiQuotes/loadListContract_id",
                    {{"variety", product}, {"trade_date", detail::compact(day)}})) {
                const auto code = detail::text(item);
                const auto contract = contract_of("GFEX", code, day);
                if (!contract || product_of(*contract) != product)
                    throw std::invalid_argument("invalid GFEX ranked contract");
                detail::unique_contract(seen, *contract);
                std::map<std::string, MemberPosition> members;
                for (std::size_t i = 0; i < std::size(metrics); ++i) {
                    const auto rows =
                        ask("interfacesWebTiMemberDealPosiQuotes/loadList",
                            {{"variety", product},
                                {"trade_date", detail::compact(day)},
                                {"trade_type", "0"},
                                {"contract_id", code},
                                {"data_type", std::to_string(i + 1)}});
                    for (const auto& row : rows) {
                        if (detail::text(row.at("contractId")) == "总计")
                            continue;
                        const auto name = detail::text(row.at("abbr"));
                        if (name.empty())
                            throw std::invalid_argument("missing GFEX ranked member");
                        auto [at, inserted] = members.try_emplace(
                            name,
                            MemberPosition{
                                .exchange = "GFEX",
                                .product = product,
                                .contract = *contract,
                                .day = day,
                                .member = name
                            });
                        const auto [quantity, change] = metrics[i];
                        if (at->second.*quantity != Cell{})
                            throw std::invalid_argument("duplicate GFEX ranked member");
                        at->second.*quantity = detail::number(row.at("todayQty"));
                        at->second.*change = detail::number(row.at("qtySub"));
                    }
                }
                const auto observed = observation_time();
                for (auto& [name, row] : members) {
                    row.observed_ms = observed;
                    result.push_back(std::move(row));
                }
            }
        }
        return result;
    } catch (const SourceError&) {
        throw;
    } catch (const std::exception& error) {
        throw SourceError(
            "invalid_data",
            "invalid GFEX member ranking: " + std::string(error.what()));
    }
}

Exchange::Parsed
Exchange::read(const std::string& table, const std::string& exchange, const std::string& day)
{
    day_of(day);
    if (!supports(table, exchange))
        throw SourceError("unsupported", "exchange source does not provide the requested table");
    check_stop(stop_);
    if (exchange == "GFEX" && table == "member_positions")
        return {.member_positions = gfex_members(day)};
    const bool archived = exchange == "CZCE" && table == "daily" && day <= "2010-08-24";
    if (archived && archive_year_ == day.substr(0, 4)) {
        try {
            return archive_.empty() ? Parsed{}
                                    : detail::parse_text_rows(exchange, day, table, archive_, true);
        } catch (const std::exception& error) {
            throw SourceError(
                "invalid_data",
                "invalid exchange daily archive: " + std::string(error.what()));
        }
    }
    const auto date = detail::compact(day);
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
        request.method = "POST";
        request.origin = "http://www.gfex.com.cn";
        request.path = table == "daily" ? "/u/interfacesWebTiDayQuotes/loadList"
                                        : "/u/interfacesWebTiFutAndOptSettle/loadList";
        request.body = "trade_date=" + date + "&trade_type=0";
        request.content_type = "application/x-www-form-urlencoded";
    } else {
        request.method = "POST";
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
    auto reply = fetch(request);
    const bool html_receipts =
        reply.status == 404 && exchange == "SHFE" && table == "warehouse_receipts";
    const bool html_inventory = reply.status == 404 && exchange == "SHFE" && table == "inventory";
    if (html_receipts) {
        request.path = "/data/tradedata/future/stockdata/dailystock_" + date + "/ZH/all.html";
        reply = fetch(request);
    } else if (html_inventory) {
        request.path = day < "2014-05-19"
            ? "/data/tradedata/future/weeklydata/" + date + "weeklystock.html"
            : "/data/tradedata/future/stockdata/weeklystock_" + date + "/ZH/all.html";
        reply = fetch(request);
    }
    if (reply.status == 404) {
        if (archived) {
            archive_year_ = day.substr(0, 4);
            archive_.clear();
        }
        return {};
    } // No publication is not proof of a closed day.
    try {
        if (html_receipts)
            return {.warehouse_receipts = detail::shfe_html_receipts(day, reply.body)};
        if (html_inventory)
            return {.inventory = detail::shfe_html_inventory(day, reply.body)};
        if (table == "price_ticks")
            return {
                .price_ticks =
                    detail::parse_czce_price_ticks(day, reply.body, request.origin + request.path)
            };
        if (exchange == "SHFE" || exchange == "INE")
            return detail::parse_shanghai(exchange, day, table, reply.body);
        if (archived) {
            auto decoded =
                detail::utf8(archive_member(reply.body, "datahistory" + day.substr(0, 4) + ".txt"));
            auto result = detail::parse_text_rows(exchange, day, table, decoded, true);
            archive_year_ = day.substr(0, 4);
            archive_ = std::move(decoded);
            return result;
        }
        if (exchange == "CZCE" || exchange == "CFFEX")
            return detail::parse_text_rows(exchange, day, table, reply.body);
        return detail::parse_portal(exchange, day, table, reply.body);
    } catch (const std::exception& error) {
        // Public files carry no credentials; name the broken field or invariant
        // so an upstream format change can be diagnosed without saving raw replies.
        throw SourceError("invalid_data", "invalid exchange " + table + " file: " + error.what());
    }
}

} // namespace asterion::warehouse::acquisition
