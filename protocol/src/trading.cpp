#include <asterion/base/decimal.hpp>
#include <asterion/futures/futures.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/protocol/trading.hpp>
#include <google/protobuf/unknown_field_set.h>
#include <charconv>
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace asterion::protocol {
namespace {

void set(proto::Decimal* target, const Json& value)
{
    target->set_units(Decimal::parse(value.get<std::string>()).raw());
}

std::string get(const proto::Decimal& value)
{
    return Decimal::from_raw(value.units()).str();
}

proto::Side side(const std::string& value)
{
    if (value == "buy")
        return proto::BUY;
    if (value == "sell")
        return proto::SELL;
    throw std::invalid_argument("invalid side");
}

std::string side(proto::Side value)
{
    if (value == proto::BUY)
        return "buy";
    if (value == proto::SELL)
        return "sell";
    throw std::invalid_argument("invalid side");
}

} // namespace

PositionSides position_sides(proto::PositionSides value)
{
    switch (value) {
        case proto::LONG_AND_SHORT:
            return PositionSides::both;
        case proto::LONG_ONLY:
            return PositionSides::long_only;
        case proto::SHORT_ONLY:
            return PositionSides::short_only;
        default:
            throw std::invalid_argument("position sides must be both, long or short");
    }
}

proto::PositionSides encode_position_sides(const Json& value)
{
    if (!value.is_string())
        throw std::invalid_argument("position sides must be both, long or short");
    switch (parse_position_sides(value.get<std::string>())) {
        case PositionSides::long_only:
            return proto::LONG_ONLY;
        case PositionSides::short_only:
            return proto::SHORT_ONLY;
        case PositionSides::both:
            break;
    }
    return proto::LONG_AND_SHORT;
}

namespace {

proto::Offset offset(const std::string& value)
{
    if (value == "open")
        return proto::OPEN;
    if (value == "close_today")
        return proto::CLOSE_TODAY;
    if (value == "close_yesterday")
        return proto::CLOSE_YESTERDAY;
    if (value == "close")
        return proto::CLOSE;
    throw std::invalid_argument("invalid offset");
}

std::string offset(proto::Offset value)
{
    if (value == proto::OPEN)
        return "open";
    if (value == proto::CLOSE_TODAY)
        return "close_today";
    if (value == proto::CLOSE_YESTERDAY)
        return "close_yesterday";
    if (value == proto::CLOSE)
        return "close";
    throw std::invalid_argument("invalid offset");
}

proto::OrderState state(const std::string& value)
{
    if (value == "accepted")
        return proto::ACCEPTED;
    if (value == "partially_filled")
        return proto::PARTIALLY_FILLED;
    if (value == "filled")
        return proto::FILLED;
    if (value == "cancelled")
        return proto::CANCELLED;
    if (value == "rejected")
        return proto::REJECTED;
    throw std::invalid_argument("invalid order state");
}

std::string state(proto::OrderState value)
{
    switch (value) {
        case proto::ACCEPTED:
            return "accepted";
        case proto::PARTIALLY_FILLED:
            return "partially_filled";
        case proto::FILLED:
            return "filled";
        case proto::CANCELLED:
            return "cancelled";
        case proto::REJECTED:
            return "rejected";
        default:
            throw std::invalid_argument("invalid order state");
    }
}

proto::Contract contract(const Json& c)
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
    proto::Contract result;
    result.set_venue(c.at("venue").get<std::string>());
    result.set_symbol(c.at("symbol").get<std::string>());
    result.set_currency(c.at("currency").get<std::string>());
    result.set_product(c.at("product").get<std::string>());
    result.set_delivery_month(c.at("delivery_month").get<std::string>());
    set(result.mutable_price_increment(), c.at("price_increment"));
    set(result.mutable_quantity_increment(), c.at("quantity_increment"));
    set(result.mutable_multiplier(), c.at("multiplier"));
    return result;
}

Json contract(const proto::Contract& c)
{
    return {
        {"venue", c.venue()},
        {"symbol", c.symbol()},
        {"currency", c.currency()},
        {"price_increment", get(c.price_increment())},
        {"quantity_increment", get(c.quantity_increment())},
        {"multiplier", get(c.multiplier())},
        {"product", c.product()},
        {"delivery_month", c.delivery_month()}
    };
}

} // namespace

proto::Costs encode_costs(const Json& c)
{
    require_fields(
        c,
        {"margin_per_lot",
            "open_fee",
            "close_today_fee",
            "close_yesterday_fee",
            "margin_rate",
            "open_fee_rate",
            "close_today_fee_rate",
            "close_yesterday_fee_rate"});
    proto::Costs result;
    set(result.mutable_margin_per_lot(), c.at("margin_per_lot"));
    set(result.mutable_open_fee(), c.at("open_fee"));
    set(result.mutable_close_today_fee(), c.at("close_today_fee"));
    set(result.mutable_close_yesterday_fee(), c.at("close_yesterday_fee"));
    set(result.mutable_margin_rate(), c.at("margin_rate"));
    set(result.mutable_open_fee_rate(), c.at("open_fee_rate"));
    set(result.mutable_close_today_fee_rate(), c.at("close_today_fee_rate"));
    set(result.mutable_close_yesterday_fee_rate(), c.at("close_yesterday_fee_rate"));
    futures_costs(result).validate();
    return result;
}

Json decode_costs(const proto::Costs& c)
{
    if (!c.has_margin_per_lot() || !c.has_open_fee() || !c.has_close_today_fee()
        || !c.has_close_yesterday_fee() || !c.has_margin_rate() || !c.has_open_fee_rate()
        || !c.has_close_today_fee_rate() || !c.has_close_yesterday_fee_rate())
        throw std::invalid_argument("missing explicit paper costs");
    return {
        {"margin_per_lot", get(c.margin_per_lot())},
        {"open_fee", get(c.open_fee())},
        {"close_today_fee", get(c.close_today_fee())},
        {"close_yesterday_fee", get(c.close_yesterday_fee())},
        {"margin_rate", get(c.margin_rate())},
        {"open_fee_rate", get(c.open_fee_rate())},
        {"close_today_fee_rate", get(c.close_today_fee_rate())},
        {"close_yesterday_fee_rate", get(c.close_yesterday_fee_rate())}
    };
}

FuturesCosts futures_costs(const proto::Costs& c)
{
    const auto value = [](const proto::Decimal& d) {
        return Decimal::from_raw(d.units());
    };
    return {
        value(c.margin_per_lot()),
        value(c.open_fee()),
        value(c.close_today_fee()),
        value(c.close_yesterday_fee()),
        value(c.margin_rate()),
        value(c.open_fee_rate()),
        value(c.close_today_fee_rate()),
        value(c.close_yesterday_fee_rate())
    };
}

proto::Costs encode_costs(const FuturesCosts& costs)
{
    costs.validate();
    proto::Costs result;
#define COST(name) result.mutable_##name()->set_units(costs.name.raw())
    COST(margin_per_lot);
    COST(open_fee);
    COST(close_today_fee);
    COST(close_yesterday_fee);
    COST(margin_rate);
    COST(open_fee_rate);
    COST(close_today_fee_rate);
    COST(close_yesterday_fee_rate);
#undef COST
    return result;
}

std::vector<FuturesCostVersion> cost_schedule(const proto::CostSchedule& input)
{
    std::vector<FuturesCostVersion> result;
    for (const auto& version : input.versions()) {
        (void)decode_costs(version.values());
        result.push_back(
            {version.effective_from(), version.source(), futures_costs(version.values())});
    }
    validate_cost_schedule(result);
    return result;
}

proto::CostSchedule encode_cost_schedule(const Json& input)
{
    if (!input.is_array() || input.empty() || input.size() > 512)
        throw std::invalid_argument("cost schedule requires 1 to 512 versions");
    proto::CostSchedule result;
    for (const auto& version : input) {
        require_fields(version, {"effective_from", "source", "values"});
        auto* row = result.add_versions();
        row->set_effective_from(version.at("effective_from").get<std::string>());
        row->set_source(version.at("source").get<std::string>());
        *row->mutable_values() = encode_costs(version.at("values"));
    }
    (void)cost_schedule(result);
    return result;
}

Json decode_cost_schedule(const proto::CostSchedule& input)
{
    (void)cost_schedule(input);
    Json result = Json::array();
    for (const auto& version : input.versions())
        result.push_back(
            {{"effective_from", version.effective_from()},
                {"source", version.source()},
                {"values", decode_costs(version.values())}});
    return result;
}

void validate_message(const google::protobuf::Message& message)
{
    const auto* reflection = message.GetReflection();
    if (reflection->GetUnknownFields(message).field_count())
        throw std::invalid_argument("unknown Protobuf field");
    // Our proto3 contracts have no extensions. Walk message fields directly;
    // enumerating every populated scalar allocates a vector for each Decimal
    // and bar in large immutable datasets, without contributing validation.
    const auto* descriptor = message.GetDescriptor();
    for (int index = 0; index < descriptor->field_count(); ++index) {
        const auto* field = descriptor->field(index);
        if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
            if (field->is_repeated()) {
                for (int i = 0; i < reflection->FieldSize(message, field); ++i)
                    validate_message(reflection->GetRepeatedMessage(message, field, i));
            } else if (reflection->HasField(message, field))
                validate_message(reflection->GetMessage(message, field));
        }
    }
}

proto::Contract encode_contract(const Json& value)
{
    return contract(value);
}

Instrument instrument(const proto::Contract& c)
{
    if (!c.has_price_increment() || !c.has_quantity_increment() || !c.has_multiplier())
        throw std::invalid_argument("incomplete contract specification");
    Instrument result{
        {c.venue(), c.symbol()},
        c.currency(),
        Decimal::from_raw(c.price_increment().units()),
        Decimal::from_raw(c.quantity_increment().units()),
        Decimal::from_raw(c.multiplier().units())
    };
    FuturesContract{result, c.product(), c.delivery_month()}.validate();
    return result;
}

Json decode_contract(const proto::Contract& value)
{
    return contract(value);
}

proto::RiskLimits encode_risk(const Json& value)
{
    require_fields(value, {"max_order_quantity", "max_gross_quantity", "max_working_orders"});
    if (!value.at("max_working_orders").is_number_unsigned() || value.at("max_working_orders") == 0)
        throw std::invalid_argument("invalid risk working order limit");
    proto::RiskLimits result;
    set(result.mutable_max_order_quantity(), value.at("max_order_quantity"));
    set(result.mutable_max_gross_quantity(), value.at("max_gross_quantity"));
    if (get(result.max_order_quantity()) != value.at("max_order_quantity").get<std::string>()
        || get(result.max_gross_quantity()) != value.at("max_gross_quantity").get<std::string>())
        throw std::invalid_argument("risk quantities require canonical decimal text");
    if (result.max_order_quantity().units() <= 0 || result.max_gross_quantity().units() <= 0)
        throw std::invalid_argument("risk quantities must be positive");
    result.set_max_working_orders(value.at("max_working_orders").get<std::uint64_t>());
    return result;
}

namespace {

void validate_risk(const proto::RiskLimits& value)
{
    if (!value.has_max_order_quantity() || !value.has_max_gross_quantity()
        || value.max_order_quantity().units() <= 0 || value.max_gross_quantity().units() <= 0
        || !value.max_working_orders())
        throw std::invalid_argument("missing explicit risk limits");
}

} // namespace

Json decode_risk(const proto::RiskLimits& value)
{
    validate_risk(value);
    return {
        {"max_order_quantity", get(value.max_order_quantity())},
        {"max_gross_quantity", get(value.max_gross_quantity())},
        {"max_working_orders", value.max_working_orders()}
    };
}

namespace {

Json instrument_fields(const std::string& venue, const std::string& symbol)
{
    InstrumentId{venue, symbol}.validate();
    return {{"venue", venue}, {"symbol", symbol}};
}

} // namespace

proto::Command encode_command(const Json& c)
{
    proto::Command result;
    result.set_request_id(c.at("request_id").get<std::string>());
    const auto action = c.at("action").get<std::string>();
    if (action == "cancel") {
        require_fields(c, {"request_id", "action", "order_id"});
        result.mutable_cancel()->set_order_id(c.at("order_id").get<std::string>());
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
        auto* order = result.mutable_submit();
        order->set_order_id(c.at("order_id").get<std::string>());
        order->set_venue(c.at("venue").get<std::string>());
        order->set_symbol(c.at("symbol").get<std::string>());
        InstrumentId{order->venue(), order->symbol()}.validate();
        order->set_side(side(c.at("side").get<std::string>()));
        order->set_offset(offset(c.at("offset").get<std::string>()));
        set(order->mutable_quantity(), c.at("quantity"));
        set(order->mutable_price(), c.at("price"));
    } else if (action == "live_authorize") {
        require_fields(c, {"request_id", "action", "user_id"});
        result.mutable_live_authorize()->set_user_id(c.at("user_id").get<std::string>());
    } else if (action == "live_revoke") {
        require_fields(c, {"request_id", "action"});
        result.mutable_live_revoke();
    } else if (action == "live_policy") {
        require_fields(c, {"request_id", "action", "policy"});
        *result.mutable_live_policy()->mutable_policy() = encode_live_policy(c.at("policy"));
    } else if (action == "live_resolve") {
        require_fields(c, {"request_id", "action", "order_id"});
        result.mutable_live_resolve()->set_order_id(c.at("order_id").get<std::string>());
    } else
        throw std::invalid_argument("unsupported trading operation");
    return result;
}

Json decode_command(const proto::Command& c)
{
    Json result{{"request_id", c.request_id()}};
    switch (c.operation_case()) {
        case proto::Command::kCancel:
            result["action"] = "cancel";
            result["order_id"] = c.cancel().order_id();
            break;
        case proto::Command::kSubmit: {
            const auto& o = c.submit();
            auto fields = instrument_fields(o.venue(), o.symbol());
            result.update(
                {{"action", "submit"},
                    {"order_id", o.order_id()},
                    {"side", side(o.side())},
                    {"offset", offset(o.offset())},
                    {"quantity", get(o.quantity())},
                    {"price", get(o.price())}});
            result.update(fields);
            break;
        }
        case proto::Command::kLiveAuthorize:
            result.update(
                {{"action", "live_authorize"}, {"user_id", c.live_authorize().user_id()}});
            break;
        case proto::Command::kLiveRevoke:
            result["action"] = "live_revoke";
            break;
        case proto::Command::kLivePolicy:
            result.update(
                {{"action", "live_policy"},
                    {"policy", decode_live_policy(c.live_policy().policy())}});
            break;
        case proto::Command::kLiveResolve:
            result.update({{"action", "live_resolve"}, {"order_id", c.live_resolve().order_id()}});
            break;
        default:
            throw std::invalid_argument("missing trading operation");
    }
    return result;
}

namespace {

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

std::string live_costs_state(const std::string& value)
{
    if (value != "querying" && value != "ready" && value != "unavailable")
        throw std::invalid_argument("invalid live costs state");
    return value;
}

std::string live_status(const std::string& value)
{
    static const std::set<std::string>
        known{"submitted", "accepted", "partially_filled", "filled", "cancelled", "rejected"};
    if (!known.contains(value))
        throw std::invalid_argument("invalid live order status");
    return value;
}

} // namespace

proto::LivePolicy encode_live_policy(const Json& policy)
{
    require_fields(policy, {"risk", "max_price_deviation", "contracts"});
    if (!policy.at("contracts").is_array())
        throw std::invalid_argument("live policy contracts must be an array");
    proto::LivePolicy result;
    *result.mutable_risk() = encode_risk(policy.at("risk"));
    set(result.mutable_max_price_deviation(), policy.at("max_price_deviation"));
    if (get(result.max_price_deviation()) != policy.at("max_price_deviation").get<std::string>())
        throw std::invalid_argument("price deviation requires canonical decimal text");
    for (const auto& c : policy.at("contracts"))
        *result.add_contracts() = contract(c);
    validate_live_policy(result);
    return result;
}

void validate_live_policy(const proto::LivePolicy& policy)
{
    if (!policy.has_risk() || policy.contracts().empty()
        || static_cast<std::size_t>(policy.contracts_size()) > max_portfolio_contracts)
        throw std::invalid_argument("live policy requires risk and 1 to 20 contracts");
    validate_risk(policy.risk());
    const auto deviation = Decimal::from_raw(policy.max_price_deviation().units());
    if (!policy.has_max_price_deviation() || deviation <= Decimal{}
        || deviation >= Decimal::parse("1"))
        throw std::invalid_argument("price deviation limit must be above 0 and below 1");
    std::set<InstrumentId> seen;
    for (const auto& c : policy.contracts()) {
        const auto terms = instrument(c);
        FuturesContract{terms, c.product(), c.delivery_month()}.validate();
        if (!seen.insert(terms.id).second)
            throw std::invalid_argument("duplicate live contract");
    }
}

Json decode_live_policy(const proto::LivePolicy& policy)
{
    validate_live_policy(policy);
    Json contracts = Json::array();
    for (const auto& value : policy.contracts())
        contracts.push_back(contract(value));
    return {
        {"risk", decode_risk(policy.risk())},
        {"max_price_deviation", get(policy.max_price_deviation())},
        {"contracts", std::move(contracts)}
    };
}

proto::TradingAccount encode_trading_account(const Json& account)
{
    require_fields(account, {"account_id", "broker", "environment"});
    validate_id(account.at("account_id").get<std::string>());
    const auto broker = live_broker(account.at("broker"));
    if (account.at("environment") != "real" && account.at("environment") != "simulation")
        throw std::invalid_argument("choose the trading account environment");
    proto::TradingAccount result;
    result.set_account_id(account.at("account_id"));
    result.set_environment(
        account.at("environment") == "real" ? proto::REAL : proto::BROKER_SIMULATION);
    result.mutable_broker()->set_front(broker.at("front"));
    result.mutable_broker()->set_broker_id(broker.at("broker_id"));
    result.mutable_broker()->set_user_id(broker.at("user_id"));
    result.mutable_broker()->set_app_id(broker.at("app_id"));
    return result;
}

Json decode_trading_account(const proto::TradingAccount& account)
{
    validate_id(account.account_id());
    if (account.environment() != proto::REAL && account.environment() != proto::BROKER_SIMULATION)
        throw std::invalid_argument("choose the trading account environment");
    return {
        {"account_id", account.account_id()},
        {"environment", account.environment() == proto::REAL ? "real" : "simulation"},
        {"broker",
            live_broker(
                {{"front", account.broker().front()},
                    {"broker_id", account.broker().broker_id()},
                    {"user_id", account.broker().user_id()},
                    {"app_id", account.broker().app_id()}})}
    };
}

proto::LiveInput encode_live_input(const Json& m)
{
    require_fields(m, {"type", "account_id", "broker", "policy", "environment"});
    if (m.at("type") != "live_ctp")
        throw std::invalid_argument("invalid live input");
    validate_id(m.at("account_id").get<std::string>());
    proto::LiveInput result;
    result.set_account_id(m.at("account_id"));
    const auto environment = m.at("environment").get<std::string>();
    if (environment != "real" && environment != "simulation")
        throw std::invalid_argument("choose the trading account environment");
    result.set_environment(environment == "real" ? proto::REAL : proto::BROKER_SIMULATION);
    const auto broker = live_broker(m.at("broker"));
    result.mutable_broker()->set_front(broker.at("front"));
    result.mutable_broker()->set_broker_id(broker.at("broker_id"));
    result.mutable_broker()->set_user_id(broker.at("user_id"));
    result.mutable_broker()->set_app_id(broker.at("app_id"));
    *result.mutable_policy() = encode_live_policy(m.at("policy"));
    return result;
}

Json decode_live_input(const proto::LiveInput& input)
{
    if (!input.has_broker() || !input.has_policy())
        throw std::invalid_argument("live input requires a broker and policy");
    validate_id(input.account_id());
    if (input.environment() != proto::REAL && input.environment() != proto::BROKER_SIMULATION)
        throw std::invalid_argument("choose the trading account environment");
    auto broker = live_broker(
        {{"front", input.broker().front()},
            {"broker_id", input.broker().broker_id()},
            {"user_id", input.broker().user_id()},
            {"app_id", input.broker().app_id()}});
    auto policy = decode_live_policy(input.policy());
    return {
        {"account_id", input.account_id()},
        {"environment", input.environment() == proto::REAL ? "real" : "simulation"},
        {"type", "live_ctp"},
        {"broker", std::move(broker)},
        {"policy", std::move(policy)}
    };
}

Json decode_live_snapshot(const proto::LiveSnapshot& s)
{
    validate_id(s.account_id());
    validate_id(s.policy_revision());
    if (s.segment_count() > 9007199254740991ULL)
        throw std::invalid_argument("invalid trading journal segment count");
    const auto& capacity = s.capacity();
    if (!s.has_capacity() || !capacity.records_limit() || !capacity.bytes_limit()
        || capacity.records_used() > capacity.records_limit()
        || capacity.bytes_used() > capacity.bytes_limit()
        || capacity.records_limit() > 9007199254740991ULL
        || capacity.bytes_limit() > 9007199254740991ULL)
        throw std::invalid_argument("invalid journal capacity");
    auto input = decode_live_input([&] {
        proto::LiveInput value;
        value.set_account_id(s.account_id());
        value.set_environment(s.environment());
        *value.mutable_broker() = s.broker();
        *value.mutable_policy()->mutable_risk() = s.risk();
        *value.mutable_policy()->mutable_max_price_deviation() = s.max_price_deviation();
        *value.mutable_policy()->mutable_contracts() = s.contracts();
        return value;
    }());
    static const std::set<std::string> phases{
        "disconnected",
        "connecting",
        "authenticating",
        "logging_in",
        "confirming",
        "synchronizing",
        "ready",
        "error"
    };
    if (!phases.contains(s.phase()))
        throw std::invalid_argument("invalid live phase");
    Json result{
        {"mode", "live"},
        {"environment", input.at("environment")},
        {"account_id", s.account_id()},
        {"policy_revision", s.policy_revision()},
        {"segment_count", s.segment_count()},
        {"broker", input.at("broker")},
        {"risk", input.at("policy").at("risk")},
        {"max_price_deviation", input.at("policy").at("max_price_deviation")},
        {"contracts", input.at("policy").at("contracts")},
        {"phase", s.phase()},
        {"error_code", s.error_code()},
        {"trading_day", s.trading_day()},
        {"synchronized_ms", s.synchronized_ms()},
        {"broker_sequence", std::to_string(s.broker_sequence())},
        {"connection_generation", std::to_string(s.connection_generation())},
        {"positions_reconciled", s.positions_reconciled()},
        {"funds", nullptr},
        {"positions", Json::array()},
        {"orders", Json::array()},
        {"trades", Json::array()},
        {"authorization", nullptr},
        {"unconfirmed", Json::array()},
        {"costs", Json::array()},
        {"storage_state", s.recovery_required() ? "recovery_required" : "ready"},
        {"capacity",
            {{"records_used", capacity.records_used()},
                {"records_limit", capacity.records_limit()},
                {"bytes_used", capacity.bytes_used()},
                {"bytes_limit", capacity.bytes_limit()}}}
    };
    for (const auto& c : s.costs()) {
        auto item = instrument_fields(c.venue(), c.symbol());
        const auto state = live_costs_state(c.state());
        if ((state == "ready") != c.has_costs())
            throw std::invalid_argument("live costs require rates exactly when ready");
        item.update(
            {{"state", state},
                {"error_code", c.error_code()},
                {"queried_ms", c.queried_ms()},
                {"costs", c.has_costs() ? decode_costs(c.costs()) : Json(nullptr)}});
        result["costs"].push_back(std::move(item));
    }
    if (s.has_funds()) {
        const auto& f = s.funds();
        result["funds"] = {
            {"balance", get(f.balance())},
            {"available", get(f.available())},
            {"margin", get(f.margin())},
            {"commission", get(f.commission())},
            {"close_profit", get(f.close_profit())},
            {"position_profit", get(f.position_profit())}
        };
    }
    for (const auto& p : s.positions()) {
        auto item = instrument_fields(p.venue(), p.symbol());
        item.update(
            {{"side", side(p.side())},
                {"today", get(p.today())},
                {"yesterday", get(p.yesterday())}});
        result["positions"].push_back(std::move(item));
    }
    for (const auto& o : s.orders()) {
        auto item = instrument_fields(o.venue(), o.symbol());
        item.update(
            {{"id", o.id()},
                {"broker_key", o.broker_key()},
                {"exchange_order_id", o.exchange_order_id()},
                {"side", side(o.side())},
                {"offset", offset(o.offset())},
                {"quantity", get(o.quantity())},
                {"filled", get(o.filled())},
                {"limit_price", get(o.limit_price())},
                {"status", live_status(o.status())},
                {"error_code", o.error_code()}});
        result["orders"].push_back(std::move(item));
    }
    for (const auto& t : s.trades()) {
        auto item = instrument_fields(t.venue(), t.symbol());
        item.update(
            {{"id", t.id()},
                {"order_id", t.order_id()},
                {"side", side(t.side())},
                {"offset", offset(t.offset())},
                {"quantity", get(t.quantity())},
                {"price", get(t.price())},
                {"trading_day", t.trading_day()},
                {"trade_time", t.trade_time()}});
        result["trades"].push_back(std::move(item));
    }
    if (s.has_authorization())
        result["authorization"] = {{"authorized_at_ms", s.authorization().authorized_at_ms()}};
    for (const auto& u : s.unconfirmed())
        result["unconfirmed"].push_back(
            {{"id", u.id()}, {"broker_key", u.broker_key()}, {"trading_day", u.trading_day()}});
    return result;
}

} // namespace asterion::protocol
