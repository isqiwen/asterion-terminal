#include "exchange_rules.hpp"
#include <asterion/base/error.hpp>
#include <asterion/base/serialization.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <regex>
#include <set>
#include <stdexcept>

namespace asterion::warehouse {
namespace fs = std::filesystem;

namespace {

constexpr std::string_view open_ended_until = "2100-01-01";

void check_day(std::string_view day, const std::string& where)
{
    static const std::regex syntax(R"(\d{4}-\d{2}-\d{2})");
    const std::string value(day);
    if (!std::regex_match(value, syntax))
        throw Error(ErrorCode::invalid_request, where + " needs YYYY-MM-DD dates");
    const std::chrono::year_month_day date{
        std::chrono::year(std::stoi(value.substr(0, 4))),
        std::chrono::month(std::stoul(value.substr(5, 2))),
        std::chrono::day(std::stoul(value.substr(8, 2)))
    };
    if (!date.ok() || value < "1990-01-01")
        throw Error(ErrorCode::invalid_request, where + " has an invalid date " + value);
}

void check_product(const std::string& product, const std::string& where)
{
    static const std::regex syntax("[a-z]{1,4}");
    if (!std::regex_match(product, syntax))
        throw Error(ErrorCode::invalid_request, where + " has an invalid product code " + product);
}

std::string string_field(const Json& object, const std::string& key, const std::string& where)
{
    if (!object.contains(key) || !object.at(key).is_string())
        throw Error(ErrorCode::invalid_request, where + " needs string " + key);
    return object.at(key).get<std::string>();
}

void exact_keys(
    const Json& object,
    const std::set<std::string>& required,
    const std::set<std::string>& optional,
    const std::string& where)
{
    if (!object.is_object())
        throw Error(ErrorCode::invalid_request, "invalid exchange rules: " + where);
    for (const auto& [key, _] : object.items())
        if (!required.contains(key) && !optional.contains(key))
            throw Error(ErrorCode::invalid_request, where + " has an unknown key " + key);
    for (const auto& key : required)
        if (!object.contains(key))
            throw Error(ErrorCode::invalid_request, where + " needs " + key);
}

} // namespace

FeeBasis DceFeeRules::basis(std::string_view product, std::string_view day) const
{
    const auto found = products.find(product);
    if (found == products.end())
        return FeeBasis::unknown;
    for (const auto& rule : found->second)
        if (rule.effective_from <= day && day < rule.effective_until)
            return rule.basis;
    return FeeBasis::unknown;
}

ExchangeRules load_exchange_rules(const fs::path& file)
{
    const auto where = file.string();
    if (!fs::is_regular_file(file) || fs::file_size(file) > 262144)
        throw Error(ErrorCode::invalid_request, "invalid exchange rules: " + where);
    std::ifstream input(file, std::ios::binary);
    const std::string raw{std::istreambuf_iterator<char>(input), {}};
    Json document;
    try {
        document = parse_json(raw, 262144);
    } catch (const std::exception&) {
        throw Error(ErrorCode::invalid_request, "invalid exchange rules: " + where);
    }
    exact_keys(document, {"price_ticks", "dce_settlement_fees"}, {}, where);

    ExchangeRules result;
    const auto& ticks_by_exchange = document.at("price_ticks");
    const std::set<std::string> known_exchanges{"SHFE", "INE", "DCE", "CZCE", "CFFEX", "GFEX"};
    exact_keys(ticks_by_exchange, known_exchanges, {}, where);
    for (const auto* exchange : {"SHFE", "INE", "DCE", "CZCE", "CFFEX", "GFEX"}) {
        const auto& section = ticks_by_exchange.at(exchange);
        exact_keys(section, {"source_url", "products"}, {}, where);
        const auto default_url = string_field(section, "source_url", where);
        if (default_url.empty())
            throw Error(ErrorCode::invalid_request, where + " needs non-empty source_url");
        const auto& products = section.at("products");
        if (!products.is_object() || products.empty())
            throw Error(
                ErrorCode::invalid_request,
                where + " needs price tick products for " + exchange);
        for (const auto& [product, rules] : products.items()) {
            check_product(product, where);
            if (!rules.is_array() || rules.empty())
                throw Error(
                    ErrorCode::invalid_request,
                    where + " needs price tick intervals for " + exchange + "/" + product);
            std::map<std::string, std::string> previous_until;
            for (const auto& item : rules) {
                exact_keys(item, {"from", "tick"}, {"until", "contract", "source_url"}, where);
                const auto from = string_field(item, "from", where);
                const auto until = item.contains("until") ? string_field(item, "until", where)
                                                          : std::string(open_ended_until);
                check_day(from, where);
                check_day(until, where);
                if (from >= until)
                    throw Error(
                        ErrorCode::invalid_request,
                        where + " has inverted price tick dates for " + exchange + "/" + product);
                Cell contract;
                std::string scope_key;
                if (item.contains("contract")) {
                    scope_key = string_field(item, "contract", where);
                    static const std::regex contract_syntax(R"([A-Z]+/[a-z]{1,4}/\d{4}-\d{2})");
                    if (!std::regex_match(scope_key, contract_syntax)
                        || !scope_key.starts_with(std::string(exchange) + "/" + product + "/"))
                        throw Error(
                            ErrorCode::invalid_request,
                            where + " has an invalid contract " + scope_key);
                    contract = scope_key;
                }
                if (const auto prior = previous_until.find(scope_key);
                    prior != previous_until.end() && from < prior->second)
                    throw Error(
                        ErrorCode::invalid_request,
                        where + " has overlapping price tick intervals for " + exchange + "/"
                            + product);
                previous_until[scope_key] = until;
                Decimal tick;
                const auto tick_text = string_field(item, "tick", where);
                try {
                    tick = Decimal::parse(tick_text);
                } catch (const std::exception&) {
                    throw Error(
                        ErrorCode::invalid_request,
                        where + " has an invalid price tick for " + exchange + "/" + product);
                }
                if (tick <= Decimal{} || tick.str() != tick_text)
                    throw Error(
                        ErrorCode::invalid_request,
                        where + " needs canonical positive price ticks for " + exchange + "/"
                            + product);
                const auto url = item.contains("source_url")
                    ? string_field(item, "source_url", where)
                    : default_url;
                if (url.empty())
                    throw Error(ErrorCode::invalid_request, where + " needs non-empty source_url");
                result.price_ticks.push_back(
                    {.exchange = exchange,
                        .product = product,
                        .contract = std::move(contract),
                        .effective_from = from,
                        .effective_until = until,
                        .price_tick = tick,
                        .source_url = url});
            }
        }
    }

    const auto& dce = document.at("dce_settlement_fees");
    exact_keys(dce, {"verified_through", "products"}, {}, where);
    result.dce_settlement_fees.verified_through = string_field(dce, "verified_through", where);
    check_day(result.dce_settlement_fees.verified_through, where);
    const auto& fee_products = dce.at("products");
    if (!fee_products.is_object() || fee_products.empty())
        throw Error(ErrorCode::invalid_request, where + " needs DCE settlement fee products");
    for (const auto& [product, rules] : fee_products.items()) {
        check_product(product, where);
        if (!rules.is_array() || rules.empty())
            throw Error(
                ErrorCode::invalid_request,
                where + " needs DCE fee intervals for " + product);
        std::string prior_until;
        auto& target = result.dce_settlement_fees.products[product];
        for (const auto& item : rules) {
            exact_keys(item, {"from", "basis"}, {"until"}, where);
            const auto from = string_field(item, "from", where);
            const auto until = item.contains("until") ? string_field(item, "until", where)
                                                      : std::string(open_ended_until);
            check_day(from, where);
            check_day(until, where);
            if (from >= until || (!prior_until.empty() && from < prior_until))
                throw Error(
                    ErrorCode::invalid_request,
                    where + " has invalid DCE fee dates for " + product);
            prior_until = until;
            const auto basis_text = string_field(item, "basis", where);
            if (basis_text != "lot" && basis_text != "rate")
                throw Error(
                    ErrorCode::invalid_request,
                    where + " needs DCE fee basis as lot or rate for " + product);
            target.push_back(
                {.effective_from = from,
                    .effective_until = until,
                    .basis = basis_text == "lot" ? FeeBasis::lot : FeeBasis::rate});
        }
    }
    return result;
}

void verify_price_ticks(
    std::span<const PriceTick> verified,
    std::span<const PriceTick> observed,
    std::string_view day)
{
    for (const auto& item : observed) {
        const auto match = std::ranges::find_if(verified, [&](const PriceTick& rule) {
            return rule.exchange == item.exchange && rule.product == item.product
                && rule.contract == item.contract && rule.effective_from <= day
                && day < rule.effective_until;
        });
        const auto target = std::holds_alternative<std::string>(item.contract)
            ? std::get<std::string>(item.contract)
            : item.exchange + "/" + item.product;
        if (match == verified.end())
            throw std::invalid_argument(
                "unverified price tick for " + target + " (" + item.price_tick.str() + ") on "
                + std::string(day) + "; update config/exchange_rules.json");
        if (match->price_tick != item.price_tick)
            throw std::invalid_argument(
                "price tick changed for " + target + " on " + std::string(day) + ": verified "
                + match->price_tick.str() + ", source " + item.price_tick.str());
    }
}

} // namespace asterion::warehouse
