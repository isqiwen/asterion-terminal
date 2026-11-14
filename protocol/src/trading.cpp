#include <asterion/base/decimal.hpp>
#include <asterion/futures/futures.hpp>
#include <asterion/protocol/trading.hpp>
#include <google/protobuf/unknown_field_set.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <set>
#include <stdexcept>

namespace asterion::protocol {
namespace {

void set(::common::Decimal* target, const Json& value)
{
    target->set_text(Decimal::parse(value.get<std::string>()).str());
}

Decimal value(const ::common::Decimal& decimal)
{
    return Decimal::parse(decimal.text());
}

std::string get(const ::common::Decimal& decimal)
{
    return value(decimal).str();
}

::common::Side side(const std::string& name)
{
    if (name == "buy")
        return ::common::SIDE_BUY;
    if (name == "sell")
        return ::common::SIDE_SELL;
    throw std::invalid_argument("invalid side");
}

std::string side(::common::Side side)
{
    if (side == ::common::SIDE_BUY)
        return "buy";
    if (side == ::common::SIDE_SELL)
        return "sell";
    throw std::invalid_argument("invalid side");
}

::common::Offset offset(const std::string& name)
{
    if (name == "open")
        return ::common::OFFSET_OPEN;
    if (name == "close_today")
        return ::common::OFFSET_CLOSE_TODAY;
    if (name == "close_yesterday")
        return ::common::OFFSET_CLOSE_YESTERDAY;
    if (name == "close")
        return ::common::OFFSET_CLOSE;
    throw std::invalid_argument("invalid offset");
}

std::string offset(::common::Offset offset)
{
    switch (offset) {
        case ::common::OFFSET_OPEN:
            return "open";
        case ::common::OFFSET_CLOSE_TODAY:
            return "close_today";
        case ::common::OFFSET_CLOSE_YESTERDAY:
            return "close_yesterday";
        case ::common::OFFSET_CLOSE:
            return "close";
        default:
            throw std::invalid_argument("invalid offset");
    }
}

// Contract terms as the journal stores them; CTP futures trade in CNY.
::trading::ContractTerms contract(const Json& c)
{
    require_fields(
        c,
        {"venue",
            "symbol",
            "currency",
            "price_increment",
            "quantity_increment",
            "multiplier",
            "product",
            "delivery_month"});
    ::trading::ContractTerms result;
    result.mutable_instrument()->set_exchange(c.at("venue").get<std::string>());
    result.mutable_instrument()->set_symbol(c.at("symbol").get<std::string>());
    result.set_product(c.at("product").get<std::string>());
    result.set_delivery_month(c.at("delivery_month").get<std::string>());
    set(result.mutable_price_tick(), c.at("price_increment"));
    set(result.mutable_lot_size(), c.at("quantity_increment"));
    set(result.mutable_multiplier(), c.at("multiplier"));
    return result;
}

Json contract(const ::trading::ContractTerms& c)
{
    return {
        {"venue", c.instrument().exchange()},
        {"symbol", c.instrument().symbol()},
        {"currency", "CNY"},
        {"price_increment", get(c.price_tick())},
        {"quantity_increment", get(c.lot_size())},
        {"multiplier", get(c.multiplier())},
        {"product", c.product()},
        {"delivery_month", c.delivery_month()}
    };
}

::common::RiskLimits encode_risk(const Json& value)
{
    require_fields(
        value,
        {"max_order_quantity",
            "max_gross_quantity",
            "max_contract_quantity",
            "max_working_orders"});
    if (!value.at("max_working_orders").is_number_unsigned() || value.at("max_working_orders") == 0)
        throw std::invalid_argument("invalid risk working order limit");
    ::common::RiskLimits result;
    set(result.mutable_max_order_lots(), value.at("max_order_quantity"));
    set(result.mutable_max_gross_lots(), value.at("max_gross_quantity"));
    set(result.mutable_max_contract_lots(), value.at("max_contract_quantity"));
    if (get(result.max_order_lots()) != value.at("max_order_quantity").get<std::string>()
        || get(result.max_gross_lots()) != value.at("max_gross_quantity").get<std::string>()
        || get(result.max_contract_lots()) != value.at("max_contract_quantity").get<std::string>())
        throw std::invalid_argument("risk quantities require canonical decimal text");
    result.set_max_working_orders(value.at("max_working_orders").get<std::uint32_t>());
    return result;
}

void validate_risk(const ::common::RiskLimits& limits)
{
    if (!limits.has_max_order_lots() || !limits.has_max_gross_lots()
        || !limits.has_max_contract_lots() || value(limits.max_order_lots()) <= Decimal{}
        || value(limits.max_gross_lots()) <= Decimal{}
        || value(limits.max_contract_lots()) <= Decimal{} || !limits.max_working_orders())
        throw std::invalid_argument("missing explicit risk limits");
    if (value(limits.max_contract_lots()) > value(limits.max_gross_lots()))
        throw std::invalid_argument("contract risk limit cannot exceed account gross limit");
}

Json decode_risk(const ::common::RiskLimits& limits)
{
    validate_risk(limits);
    return {
        {"max_order_quantity", get(limits.max_order_lots())},
        {"max_gross_quantity", get(limits.max_gross_lots())},
        {"max_contract_quantity", get(limits.max_contract_lots())},
        {"max_working_orders", limits.max_working_orders()}
    };
}

// CTP identities: short printable text without spaces.
std::string broker_text(const Json& value, const char* name, std::size_t limit)
{
    const auto text = value.at(name).get<std::string>();
    if (text.empty() || text.size() > limit || std::ranges::any_of(text, [](unsigned char c) {
            return c <= ' ' || c > '~';
        }))
        throw std::invalid_argument(std::string("invalid live broker ") + name);
    return text;
}

Json live_broker(const Json& b)
{
    require_fields(b, {"front", "broker_id", "user_id", "app_id"});
    const auto front = broker_text(b, "front", 64);
    const auto colon = front.rfind(':');
    const auto host = front.substr(0, colon);
    const auto port = colon == std::string::npos ? std::string{} : front.substr(colon + 1);
    int number = 0;
    const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
    if (!front.starts_with("tcp://") || host.size() <= 6 || error != std::errc{}
        || end != port.data() + port.size() || number < 1 || number > 65535
        || std::ranges::any_of(host.substr(6), [](unsigned char c) {
               return !std::isalnum(c) && c != '.' && c != '-';
           }))
        throw std::invalid_argument("live broker front must look like tcp://host:port");
    return {
        {"front", front},
        {"broker_id", broker_text(b, "broker_id", 10)},
        {"user_id", broker_text(b, "user_id", 15)},
        {"app_id", broker_text(b, "app_id", 32)}
    };
}

std::string environment(const Json& value)
{
    if (value != "real" && value != "simulation")
        throw std::invalid_argument("choose the trading account environment");
    return value.get<std::string>();
}

} // namespace

Instrument terms(const ::trading::ContractTerms& c)
{
    if (!c.has_price_tick() || !c.has_lot_size() || !c.has_multiplier())
        throw std::invalid_argument("incomplete contract specification");
    Instrument result{
        {c.instrument().exchange(), c.instrument().symbol()},
        "CNY",
        value(c.price_tick()),
        value(c.lot_size()),
        value(c.multiplier())
    };
    FuturesContract{result, c.product(), c.delivery_month()}.validate();
    return result;
}

::common::Costs encode_costs(const FuturesCosts& costs)
{
    costs.validate();
    ::common::Costs result;
#define COST(name) result.mutable_##name()->set_text(costs.name.str())
    COST(margin_per_lot);
    COST(margin_rate);
    COST(open_fee);
    COST(open_fee_rate);
    COST(close_today_fee);
    COST(close_today_fee_rate);
    COST(close_yesterday_fee);
    COST(close_yesterday_fee_rate);
#undef COST
    return result;
}

::trading::Command encode_command(const Json& c)
{
    ::trading::Command result;
    result.set_request_id(c.at("request_id").get<std::string>());
    const auto action = c.at("action").get<std::string>();
    if (action == "cancel") {
        require_fields(c, {"request_id", "action", "order_id"});
        result.mutable_cancel_order()->set_id(c.at("order_id").get<std::string>());
    } else if (action == "submit") {
        require_fields(
            c,
            {"request_id",
                "action",
                "order_id",
                "venue",
                "symbol",
                "side",
                "offset",
                "quantity",
                "price"});
        auto* order = result.mutable_submit_order();
        order->set_order_id(c.at("order_id").get<std::string>());
        order->mutable_instrument()->set_exchange(c.at("venue").get<std::string>());
        order->mutable_instrument()->set_symbol(c.at("symbol").get<std::string>());
        InstrumentId{order->instrument().exchange(), order->instrument().symbol()}.validate();
        order->set_side(side(c.at("side").get<std::string>()));
        order->set_offset(offset(c.at("offset").get<std::string>()));
        set(order->mutable_lots(), c.at("quantity"));
        set(order->mutable_price(), c.at("price"));
    } else if (action == "live_authorize") {
        require_fields(c, {"request_id", "action", "user_id"});
        result.mutable_authorize()->set_user_id(c.at("user_id").get<std::string>());
    } else if (action == "live_revoke") {
        require_fields(c, {"request_id", "action"});
        result.mutable_revoke();
    } else if (action == "live_policy") {
        require_fields(c, {"request_id", "action", "policy"});
        *result.mutable_change_policy() = encode_policy(c.at("policy"));
    } else if (action == "live_resolve") {
        require_fields(c, {"request_id", "action", "order_id"});
        result.mutable_resolve_order()->set_id(c.at("order_id").get<std::string>());
    } else
        throw std::invalid_argument("unsupported trading operation");
    return result;
}

::trading::Trade encode_trade(const Json& t)
{
    require_fields(
        t,
        {"id",
            "order_id",
            "venue",
            "symbol",
            "side",
            "offset",
            "quantity",
            "price",
            "trading_day",
            "trade_time",
            "received_ms"});
    ::trading::Trade result;
    result.set_id(t.at("id").get<std::string>());
    result.set_order_id(t.at("order_id").get<std::string>());
    result.mutable_instrument()->set_exchange(t.at("venue").get<std::string>());
    result.mutable_instrument()->set_symbol(t.at("symbol").get<std::string>());
    InstrumentId{result.instrument().exchange(), result.instrument().symbol()}.validate();
    result.set_side(side(t.at("side").get<std::string>()));
    result.set_offset(offset(t.at("offset").get<std::string>()));
    set(result.mutable_lots(), t.at("quantity"));
    set(result.mutable_price(), t.at("price"));
    result.set_trading_day(t.at("trading_day").get<std::string>());
    result.set_trade_time(t.at("trade_time").get<std::string>());
    result.set_received_ms(t.at("received_ms").get<std::int64_t>());
    if (result.id().empty() || result.trading_day().size() != 10)
        throw std::invalid_argument("invalid recorded trade");
    return result;
}

Json decode_trade(const ::trading::Trade& t)
{
    return {
        {"id", t.id()},
        {"order_id", t.order_id()},
        {"venue", t.instrument().exchange()},
        {"symbol", t.instrument().symbol()},
        {"side", side(t.side())},
        {"offset", offset(t.offset())},
        {"quantity", get(t.lots())},
        {"price", get(t.price())},
        {"trading_day", t.trading_day()},
        {"trade_time", t.trade_time()},
        {"received_ms", t.received_ms()}
    };
}

Json decode_command(const ::trading::Command& c)
{
    Json result{{"request_id", c.request_id()}};
    switch (c.action_case()) {
        case ::trading::Command::kCancelOrder:
            result["action"] = "cancel";
            result["order_id"] = c.cancel_order().id();
            break;
        case ::trading::Command::kSubmitOrder: {
            const auto& o = c.submit_order();
            InstrumentId{o.instrument().exchange(), o.instrument().symbol()}.validate();
            result.update(
                {{"action", "submit"},
                    {"order_id", o.order_id()},
                    {"venue", o.instrument().exchange()},
                    {"symbol", o.instrument().symbol()},
                    {"side", side(o.side())},
                    {"offset", offset(o.offset())},
                    {"quantity", get(o.lots())},
                    {"price", get(o.price())}});
            break;
        }
        case ::trading::Command::kAuthorize:
            result.update({{"action", "live_authorize"}, {"user_id", c.authorize().user_id()}});
            break;
        case ::trading::Command::kRevoke:
            result["action"] = "live_revoke";
            break;
        case ::trading::Command::kChangePolicy:
            result.update(
                {{"action", "live_policy"}, {"policy", decode_policy(c.change_policy())}});
            break;
        case ::trading::Command::kResolveOrder:
            result.update({{"action", "live_resolve"}, {"order_id", c.resolve_order().id()}});
            break;
        default:
            throw std::invalid_argument("missing trading operation");
    }
    return result;
}

::trading::Policy encode_policy(const Json& policy)
{
    require_fields(policy, {"risk", "max_price_deviation", "contracts"});
    if (!policy.at("contracts").is_array())
        throw std::invalid_argument("live policy contracts must be an array");
    ::trading::Policy result;
    *result.mutable_limits() = encode_risk(policy.at("risk"));
    set(result.mutable_max_price_deviation(), policy.at("max_price_deviation"));
    if (get(result.max_price_deviation()) != policy.at("max_price_deviation").get<std::string>())
        throw std::invalid_argument("price deviation requires canonical decimal text");
    for (const auto& c : policy.at("contracts"))
        *result.add_contracts() = contract(c);
    validate_policy(result);
    return result;
}

void validate_policy(const ::trading::Policy& policy)
{
    if (!policy.has_limits() || policy.contracts().empty()
        || static_cast<std::size_t>(policy.contracts_size()) > max_portfolio_contracts)
        throw std::invalid_argument("live policy requires risk and 1 to 20 contracts");
    validate_risk(policy.limits());
    if (!policy.has_max_price_deviation())
        throw std::invalid_argument("price deviation limit must be above 0 and below 1");
    const auto deviation = value(policy.max_price_deviation());
    if (deviation <= Decimal{} || deviation >= Decimal::parse("1"))
        throw std::invalid_argument("price deviation limit must be above 0 and below 1");
    std::set<InstrumentId> seen;
    for (const auto& c : policy.contracts())
        if (!seen.insert(terms(c).id).second)
            throw std::invalid_argument("duplicate live contract");
}

Json decode_policy(const ::trading::Policy& policy)
{
    validate_policy(policy);
    Json contracts = Json::array();
    for (const auto& value : policy.contracts())
        contracts.push_back(contract(value));
    return {
        {"risk", decode_risk(policy.limits())},
        {"max_price_deviation", get(policy.max_price_deviation())},
        {"contracts", std::move(contracts)}
    };
}

::trading::Account encode_account(const Json& account)
{
    require_fields(account, {"account_id", "broker", "environment"});
    validate_id(account.at("account_id").get<std::string>());
    const auto broker = live_broker(account.at("broker"));
    ::trading::Account result;
    result.set_id(account.at("account_id"));
    result.set_environment(
        environment(account.at("environment")) == "real" ? ::trading::ENVIRONMENT_REAL
                                                         : ::trading::ENVIRONMENT_SIMULATION);
    result.mutable_broker()->set_front(broker.at("front"));
    result.mutable_broker()->set_broker_id(broker.at("broker_id"));
    result.mutable_broker()->set_user_id(broker.at("user_id"));
    result.mutable_broker()->set_app_id(broker.at("app_id"));
    return result;
}

Json live_input(const Json& m)
{
    require_fields(m, {"type", "account_id", "broker", "policy", "environment"});
    if (m.at("type") != "live_ctp")
        throw std::invalid_argument("invalid live input");
    validate_id(m.at("account_id").get<std::string>());
    return {
        {"account_id", m.at("account_id")},
        {"environment", environment(m.at("environment"))},
        {"type", "live_ctp"},
        {"broker", live_broker(m.at("broker"))},
        {"policy", decode_policy(encode_policy(m.at("policy")))}
    };
}

::trading::Progress encode_progress(Progress::Observation observation)
{
    ::trading::Progress result;
    result.set_observed(observation.observed);
    result.set_pending(observation.pending);
    result.set_idle_ms(observation.age_ms);
    return result;
}

void validate_message(const google::protobuf::Message& message)
{
    const auto* reflection = message.GetReflection();
    if (reflection->GetUnknownFields(message).field_count())
        throw std::invalid_argument("unknown Protobuf field");
    const auto* descriptor = message.GetDescriptor();
    for (int index = 0; index < descriptor->field_count(); ++index) {
        const auto* field = descriptor->field(index);
        if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE
            && !field->is_map()) {
            if (field->is_repeated()) {
                for (int i = 0; i < reflection->FieldSize(message, field); ++i)
                    validate_message(reflection->GetRepeatedMessage(message, field, i));
            } else if (reflection->HasField(message, field))
                validate_message(reflection->GetMessage(message, field));
        }
    }
}

} // namespace asterion::protocol
