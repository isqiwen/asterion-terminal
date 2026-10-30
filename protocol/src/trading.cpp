#include <asterion/foundation/decimal.hpp>
#include <asterion/domain/futures.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/kernel/process/artifact.hpp>
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

constexpr std::pair<proto::Factor, const char*> factors[] = {
    {proto::PRICE_MOMENTUM, "momentum"},
    {proto::TERM_STRUCTURE, "term_structure"},
    {proto::VOLATILITY, "volatility"}
};
// A rule that ranks by a factor is of the kind named after it.
const char cross_prefix[] = "cross_";

} // namespace

proto::Factor encode_factor(const Json& name)
{
    for (const auto& [factor, known] : factors)
        if (name == known)
            return factor;
    throw std::invalid_argument("unknown factor");
}

const char* factor_name(proto::Factor factor)
{
    for (const auto& [known, name] : factors)
        if (known == factor)
            return name;
    throw std::invalid_argument("unknown factor");
}

proto::Strategy encode_strategy(const Json& value)
{
    if (!value.is_object() || !value.contains("rule") || !value.at("rule").is_object()
        || !value.at("rule").contains("kind") || !value.at("rule").at("kind").is_string())
        throw std::invalid_argument("invalid strategy");
    const auto& rule = value.at("rule");
    const auto kind = rule.at("kind").get<std::string>();
    // A rule over several contracts carries its own size and takes no quantity.
    const bool ranks = kind.starts_with(cross_prefix);
    const bool portfolio = kind == "fixed_weights";
    if (ranks || portfolio)
        require_fields(value, {"sides", "rule"});
    else
        require_fields(value, {"quantity", "sides", "rule"});
    const auto amount = [](const Json& text) {
        if (!text.is_string())
            throw std::invalid_argument("invalid strategy");
        return Decimal::parse(text.get<std::string>()).raw();
    };
    proto::Strategy result;
    if (!ranks && !portfolio)
        result.mutable_quantity()->set_units(amount(value.at("quantity")));
    result.set_sides(encode_position_sides(value.at("sides")));
    const auto window = [&](const char* name) {
        if (!rule.at(name).is_number_integer() || rule.at(name) < 0 || rule.at(name) > 10000)
            throw std::invalid_argument("strategy windows are whole bars up to 10000");
        return rule.at(name).get<unsigned>();
    };
    if (portfolio) {
        require_fields(
            rule,
            {"kind", "members", "gross_weight_limit", "net_weight_limit", "rebalance"});
        if (!rule.at("members").is_array() || rule.at("members").size() < 2
            || rule.at("members").size() > 20)
            throw std::invalid_argument(
                "fixed-weight policy requires 2 to 20 explicit contract weights");
        auto* target = result.mutable_fixed_weights();
        target->set_rebalance(window("rebalance"));
        target->mutable_gross_weight_limit()->set_units(amount(rule.at("gross_weight_limit")));
        target->mutable_net_weight_limit()->set_units(amount(rule.at("net_weight_limit")));
        for (const auto& member : rule.at("members")) {
            require_fields(member, {"venue", "symbol", "weight"});
            auto* fixed = target->add_members();
            fixed->set_venue(member.at("venue").get<std::string>());
            fixed->set_symbol(member.at("symbol").get<std::string>());
            fixed->mutable_weight()->set_units(amount(member.at("weight")));
        }
    } else if (kind == "moving_average") {
        require_fields(rule, {"kind", "fast", "slow"});
        result.mutable_moving_average()->set_fast(window("fast"));
        result.mutable_moving_average()->set_slow(window("slow"));
    } else if (kind == "breakout") {
        require_fields(rule, {"kind", "entry", "exit"});
        result.mutable_breakout()->set_entry(window("entry"));
        result.mutable_breakout()->set_exit(window("exit"));
    } else if (kind == "momentum") {
        require_fields(rule, {"kind", "lookback"});
        result.mutable_momentum()->set_lookback(window("lookback"));
    } else if (kind == "reversion") {
        require_fields(rule, {"kind", "window", "width"});
        result.mutable_reversion()->set_window(window("window"));
        result.mutable_reversion()->mutable_width()->set_units(amount(rule.at("width")));
    } else if (ranks) {
        require_fields(
            rule,
            {"kind", "reverse", "lookback", "rebalance", "count", "notional", "volatility"});
        if (!rule.at("reverse").is_boolean())
            throw std::invalid_argument("invalid strategy");
        auto* cross = result.mutable_cross();
        cross->set_factor(encode_factor(kind.substr(sizeof cross_prefix - 1)));
        cross->set_reversed(rule.at("reverse").get<bool>());
        cross->set_lookback(window("lookback"));
        cross->set_rebalance(window("rebalance"));
        cross->set_count(window("count"));
        cross->mutable_notional()->set_units(amount(rule.at("notional")));
        cross->set_volatility(window("volatility"));
    } else
        throw std::invalid_argument("unknown strategy rule");
    return result;
}

Json decode_strategy(const proto::Strategy& strategy)
{
    Json rule;
    switch (strategy.rule_case()) {
        case proto::Strategy::kMovingAverage:
            rule = {
                {"kind", "moving_average"},
                {"fast", strategy.moving_average().fast()},
                {"slow", strategy.moving_average().slow()}
            };
            break;
        case proto::Strategy::kBreakout:
            rule = {
                {"kind", "breakout"},
                {"entry", strategy.breakout().entry()},
                {"exit", strategy.breakout().exit()}
            };
            break;
        case proto::Strategy::kMomentum:
            rule = {{"kind", "momentum"}, {"lookback", strategy.momentum().lookback()}};
            break;
        case proto::Strategy::kReversion:
            rule = {
                {"kind", "reversion"},
                {"window", strategy.reversion().window()},
                {"width", Decimal::from_raw(strategy.reversion().width().units()).str()}
            };
            break;
        case proto::Strategy::kFixedWeights: {
            Json members = Json::array();
            for (const auto& m : strategy.fixed_weights().members())
                members.push_back(
                    {{"venue", m.venue()},
                        {"symbol", m.symbol()},
                        {"weight", Decimal::from_raw(m.weight().units()).str()}});
            rule = {
                {"kind", "fixed_weights"},
                {"rebalance", strategy.fixed_weights().rebalance()},
                {"gross_weight_limit",
                    Decimal::from_raw(strategy.fixed_weights().gross_weight_limit().units()).str()},
                {"net_weight_limit",
                    Decimal::from_raw(strategy.fixed_weights().net_weight_limit().units()).str()},
                {"members", std::move(members)}
            };
            break;
        }
        case proto::Strategy::kCross:
            rule = {
                {"kind", std::string(cross_prefix) + factor_name(strategy.cross().factor())},
                {"reverse", strategy.cross().reversed()},
                {"lookback", strategy.cross().lookback()},
                {"rebalance", strategy.cross().rebalance()},
                {"count", strategy.cross().count()},
                {"notional", Decimal::from_raw(strategy.cross().notional().units()).str()},
                {"volatility", strategy.cross().volatility()}
            };
            break;
        case proto::Strategy::RULE_NOT_SET:
            throw std::invalid_argument("unknown strategy rule");
    }
    Json result{
        {"sides", position_sides_name(position_sides(strategy.sides()))},
        {"rule", std::move(rule)}
    };
    if (!strategy.has_cross() && !strategy.has_fixed_weights())
        result["quantity"] = Decimal::from_raw(strategy.quantity().units()).str();
    return result;
}

void validate_strategy(const proto::Strategy& strategy, Decimal quantity_increment)
{
    static_cast<void>(position_sides(strategy.sides()));
    if (strategy.has_fixed_weights()) {
        const auto& rule = strategy.fixed_weights();
        if (strategy.has_quantity() || strategy.sides() != proto::LONG_AND_SHORT
            || quantity_increment != Decimal::parse("1") || !rule.rebalance()
            || rule.rebalance() > 10000 || rule.members_size() < 2 || rule.members_size() > 20
            || !rule.has_gross_weight_limit() || !rule.has_net_weight_limit()
            || rule.gross_weight_limit().units() < 0 || rule.net_weight_limit().units() < 0)
            throw std::invalid_argument(
                "fixed-weight policy requires explicit budgets, both signed directions, whole-lot contracts and a rebalance of 1..10000 bars");
        std::set<std::pair<std::string, std::string>> identities;
        Decimal gross, net;
        for (const auto& m : rule.members()) {
            if (m.venue().empty() || m.symbol().empty() || !m.has_weight()
                || !identities.emplace(m.venue(), m.symbol()).second)
                throw std::invalid_argument(
                    "fixed-weight policy requires unique actual contracts and explicit weights");
            const auto weight = Decimal::from_raw(m.weight().units());
            gross = gross + (weight < Decimal{} ? Decimal{} - weight : weight);
            net = net + weight;
        }
        if (gross > Decimal::from_raw(rule.gross_weight_limit().units())
            || (net < Decimal{} ? Decimal{} - net : net)
                > Decimal::from_raw(rule.net_weight_limit().units()))
            throw std::invalid_argument(
                "fixed-weight targets exceed the declared gross or net nominal budget");
        return;
    }
    if (strategy.has_cross()) {
        const auto& rule = strategy.cross();
        if (strategy.has_quantity() || rule.factor() == proto::FACTOR_UNSPECIFIED
            || !rule.lookback() || rule.lookback() > 10000 || !rule.rebalance()
            || rule.rebalance() > 10000 || !rule.count() || rule.count() > 10
            || rule.notional().units() <= 0)
            throw std::invalid_argument(
                "a ranking rule requires a factor, a lookback and a rebalance of 1..10000 bars, 1..10 " "contracts a side and a positive notional in place of a quantity");
        static_cast<void>(factor_name(rule.factor()));
        // A standard deviation needs two returns.
        if (rule.factor() == proto::VOLATILITY && rule.lookback() < 2)
            throw std::invalid_argument("volatility lookback must be 2..10000 observations");
        if (rule.volatility() == 1 || rule.volatility() > 10000)
            throw std::invalid_argument(
                "a ranking rule sizes by volatility over 2..10000 bars, or over none");
        return;
    }
    const auto quantity = Decimal::from_raw(strategy.quantity().units());
    if (!strategy.has_quantity() || quantity <= Decimal{}
        || !quantity.multiple_of(quantity_increment))
        throw std::invalid_argument("strategy requires a positive lot-aligned quantity");
    switch (strategy.rule_case()) {
        case proto::Strategy::kMovingAverage:
            if (const auto& rule = strategy.moving_average();
                !rule.fast() || rule.fast() >= rule.slow() || rule.slow() > 10000)
                throw std::invalid_argument("moving average requires 0 < fast < slow <= 10000");
            return;
        case proto::Strategy::kBreakout:
            if (const auto& rule = strategy.breakout();
                !rule.exit() || rule.exit() > rule.entry() || rule.entry() > 10000)
                throw std::invalid_argument("breakout requires 1 <= exit <= entry <= 10000");
            return;
        case proto::Strategy::kMomentum:
            if (!strategy.momentum().lookback() || strategy.momentum().lookback() > 10000)
                throw std::invalid_argument("momentum requires a lookback of 1..10000 bars");
            return;
        case proto::Strategy::kReversion: {
            const auto& rule = strategy.reversion();
            const auto width = Decimal::from_raw(rule.width().units());
            if (rule.window() < 2 || rule.window() > 10000 || !rule.has_width()
                || width <= Decimal{} || width > Decimal::parse("10"))
                throw std::invalid_argument(
                    "reversion requires a window of 2..10000 bars and a width above 0 up to 10");
            return;
        }
        case proto::Strategy::kFixedWeights:
        case proto::Strategy::kCross:
        case proto::Strategy::RULE_NOT_SET:
            break;
    }
    throw std::invalid_argument("unknown strategy rule");
}

std::size_t strategy_warmup(const proto::Strategy& strategy)
{
    switch (strategy.rule_case()) {
        case proto::Strategy::kMovingAverage:
            return strategy.moving_average().slow();
        case proto::Strategy::kBreakout:
            return strategy.breakout().entry() + 1;
        case proto::Strategy::kMomentum:
            return strategy.momentum().lookback() + 1;
        case proto::Strategy::kReversion:
            return strategy.reversion().window();
        case proto::Strategy::kFixedWeights:
            return 1;
        case proto::Strategy::kCross:
            // Momentum compares with the bar a lookback earlier and volatility reads
            // a lookback of returns; the term structure averages over the lookback
            // itself. Sizing by volatility needs one bar more than its window of
            // returns.
            return std::max<std::size_t>(
                strategy.cross().lookback()
                    + (strategy.cross().factor() == proto::TERM_STRUCTURE ? 0 : 1),
                strategy.cross().volatility() ? strategy.cross().volatility() + 1 : 0);
        case proto::Strategy::RULE_NOT_SET:
            break;
    }
    throw std::invalid_argument("unknown strategy rule");
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

// Identity of a portfolio's data: its datasets' revisions in contract order.
std::string dataset_revision(const proto::PaperInput& input)
{
    if (input.contracts().empty())
        throw std::invalid_argument("paper input has no contracts");
    Json revisions = Json::array();
    for (const auto& contract : input.contracts()) {
        validate_bar_dataset(contract.dataset());
        revisions.push_back(contract.dataset().revision());
    }
    return input.contracts_size() == 1 ? input.contracts(0).dataset().revision()
                                       : sha256_bytes(revisions.dump());
}

ContractTerms contract_terms(const proto::PaperContract& contract)
{
    validate_bar_dataset(contract.dataset());
    return {
        instrument(contract.dataset().contract()),
        costs_on(cost_schedule(contract.cost_schedule()), contract.dataset().bars(0).trading_day())
            .values
    };
}

// Each manifest contract pins a complete dated cost schedule.
Json decode_input(const proto::PaperInput& input, DatasetView view)
{
    if (!input.has_deposit() || !input.has_risk() || input.contracts().empty()
        || static_cast<std::size_t>(input.contracts_size()) > max_account_contracts)
        throw std::invalid_argument("paper input requires deposit, risk and 1 to 256 contracts");
    // Evaluated before the braced initializer: GCC < 13 leaks already-built
    // initializer_list elements when a later element throws (PR66139).
    auto risk = decode_risk(input.risk());
    Json contracts = Json::array();
    std::vector<ContractTerms> terms;
    for (const auto& c : input.contracts()) {
        if (!c.has_dataset() || !c.has_cost_schedule())
            throw std::invalid_argument("missing explicit paper costs or dataset");
        if (c.slippage_ticks() > max_slippage_ticks)
            throw std::invalid_argument("slippage is 0 to 100 price increments");
        contracts.push_back(
            {{"dataset", decode_bar_dataset(c.dataset(), view)},
                {"cost_schedule", decode_cost_schedule(c.cost_schedule())},
                {"slippage_ticks", c.slippage_ticks()}});
        terms.push_back(contract_terms(c));
    }
    // The account validates currency, uniqueness and each contract's terms.
    static_cast<void>(FuturesAccount(Decimal::from_raw(input.deposit().units()), terms));
    return {
        {"type", "historical_paper"},
        {"deposit", get(input.deposit())},
        {"risk", std::move(risk)},
        {"contracts", std::move(contracts)}
    };
}

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
        require_fields(c, {"request_id", "action", "policy", "risk_artifact"});
        *result.mutable_live_policy()->mutable_policy() = encode_live_policy(c.at("policy"));
        result.mutable_live_policy()->set_risk_artifact(c.at("risk_artifact").get<std::string>());
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
                    {"policy", decode_live_policy(c.live_policy().policy())},
                    {"risk_artifact", c.live_policy().risk_artifact()}});
            break;
        case proto::Command::kLiveResolve:
            result.update({{"action", "live_resolve"}, {"order_id", c.live_resolve().order_id()}});
            break;
        default:
            throw std::invalid_argument("missing trading operation");
    }
    return result;
}

proto::Position encode_position(const PositionLot& p)
{
    proto::Position result;
    auto* item = &result;
    item->set_venue(p.instrument.venue);
    item->set_symbol(p.instrument.symbol);
    item->set_side(p.side == Side::buy ? proto::BUY : proto::SELL);
    item->set_today(p.today);
    item->mutable_quantity()->set_units(p.quantity.raw());
    item->mutable_basis()->set_units(p.price.raw());
    return result;
}

proto::Order encode_order(const AccountSnapshot::OrderView& o)
{
    proto::Order result;
    auto* item = &result;
    item->set_id(o.request.id);
    item->set_venue(o.request.instrument.venue);
    item->set_symbol(o.request.instrument.symbol);
    item->set_side(o.request.side == Side::buy ? proto::BUY : proto::SELL);
    switch (o.offset) {
        case Offset::open:
            item->set_offset(proto::OPEN);
            break;
        case Offset::close_today:
            item->set_offset(proto::CLOSE_TODAY);
            break;
        case Offset::close_yesterday:
            item->set_offset(proto::CLOSE_YESTERDAY);
            break;
        case Offset::close:
            item->set_offset(proto::CLOSE);
            break;
    }
    item->mutable_quantity()->set_units(o.request.quantity.raw());
    item->mutable_limit_price()->set_units(o.request.limit_price.raw());
    item->mutable_filled()->set_units(o.filled.raw());
    switch (o.state) {
        case OrderState::accepted:
            item->set_state(proto::ACCEPTED);
            break;
        case OrderState::partially_filled:
            item->set_state(proto::PARTIALLY_FILLED);
            break;
        case OrderState::filled:
            item->set_state(proto::FILLED);
            break;
        case OrderState::cancelled:
            item->set_state(proto::CANCELLED);
            break;
        case OrderState::rejected:
            item->set_state(proto::REJECTED);
            break;
        case OrderState::pending:
            throw std::logic_error("account observation contains an unaccepted order");
    }
    return result;
}

proto::Snapshot encode_snapshot(const FuturesAccount& account, const proto::PaperInput& input)
{
    const auto s = account.snapshot();
    proto::Snapshot result;
    *result.mutable_risk() = input.risk();
    if (s.marks.size() != static_cast<std::size_t>(input.contracts_size()))
        throw std::logic_error("account contracts differ from fixed input");
    for (std::size_t i = 0; i < s.marks.size(); ++i) {
        const auto& source = input.contracts(static_cast<int>(i));
        const auto& identity = s.marks[i].instrument;
        if (identity.venue != source.dataset().contract().venue()
            || identity.symbol != source.dataset().contract().symbol())
            throw std::logic_error("account contracts differ from fixed input");
        auto* item = result.add_contracts();
        *item->mutable_contract() = source.dataset().contract();
        *item->mutable_costs() = encode_costs(account.contracts()[i].costs);
        *item->mutable_cost_schedule() = source.cost_schedule();
        item->mutable_mark()->set_units(s.marks[i].price.raw());
    }
#define VALUE(name) result.mutable_##name()->set_units(s.name.raw())
    VALUE(balance);
    VALUE(equity);
    VALUE(available);
    VALUE(margin);
    VALUE(frozen);
    VALUE(fees);
    VALUE(realized);
    VALUE(unrealized);
#undef VALUE
    for (const auto& p : s.positions)
        *result.add_positions() = encode_position(p);
    for (const auto& o : s.orders)
        *result.add_orders() = encode_order(o);
    for (const auto& f : s.fills) {
        auto* item = result.add_fills();
        item->set_id(f.execution.execution_id);
        item->set_order_id(f.execution.order_id);
        item->set_venue(f.instrument.venue);
        item->set_symbol(f.instrument.symbol);
        item->mutable_quantity()->set_units(f.execution.quantity.raw());
        item->mutable_price()->set_units(f.execution.price.raw());
        item->mutable_fee()->set_units(f.fee.raw());
    }
    return result;
}

Json decode_position(const proto::Position& p)
{
    auto item = instrument_fields(p.venue(), p.symbol());
    item.update(
        {{"side", side(p.side())},
            {"bucket", p.today() ? "today" : "yesterday"},
            {"quantity", get(p.quantity())},
            {"basis", get(p.basis())}});
    return item;
}

Json decode_order(const proto::Order& o)
{
    auto item = instrument_fields(o.venue(), o.symbol());
    item.update(
        {{"id", o.id()},
            {"side", side(o.side())},
            {"offset", offset(o.offset())},
            {"quantity", get(o.quantity())},
            {"limit_price", get(o.limit_price())},
            {"filled", get(o.filled())},
            {"state", state(o.state())}});
    return item;
}

Json decode_snapshot(const proto::Snapshot& s)
{
    if (s.contracts().empty()
        || static_cast<std::size_t>(s.contracts_size()) > max_account_contracts || !s.has_balance()
        || !s.has_equity() || !s.has_available() || !s.has_margin() || !s.has_frozen()
        || !s.has_fees() || !s.has_realized() || !s.has_unrealized() || s.total() == 0
        || s.total() > max_dataset_bars || s.cursor() > s.total()
        || (s.cursor() == 0) == s.has_timestamp_ns())
        throw std::invalid_argument("incomplete trading snapshot");
    // Evaluated before the braced initializer: GCC < 13 leaks already-built
    // initializer_list elements when a later element throws (PR66139).
    auto risk = decode_risk(s.risk());
    Json contracts = Json::array();
    for (const auto& c : s.contracts()) {
        if (!c.has_contract() || !c.has_costs() || !c.has_mark())
            throw std::invalid_argument("incomplete trading snapshot");
        contracts.push_back(
            {{"contract", contract(c.contract())},
                {"costs", decode_costs(c.costs())},
                {"cost_schedule", decode_cost_schedule(c.cost_schedule())},
                {"mark", get(c.mark())}});
    }
    Json result{
        {"risk", std::move(risk)},
        {"contracts", std::move(contracts)},
        {"mode", "historical_paper"},
        {"persistent", true},
        {"storage_state", s.recovery_required() ? "recovery_required" : "ready"},
        {"cursor", s.cursor()},
        {"total", s.total()},
        {"timestamp_ns",
            s.has_timestamp_ns() ? Json(std::to_string(s.timestamp_ns())) : Json(nullptr)}
    };
#define VALUE(name) result[#name] = get(s.name())
    VALUE(balance);
    VALUE(equity);
    VALUE(available);
    VALUE(margin);
    VALUE(frozen);
    VALUE(fees);
    VALUE(realized);
    VALUE(unrealized);
#undef VALUE
    result["positions"] = Json::array();
    result["orders"] = Json::array();
    result["fills"] = Json::array();
    // Enum decoding rejects unknown values; it runs before each braced
    // initializer (GCC < 13 PR66139 leak).
    for (const auto& p : s.positions())
        result["positions"].push_back(decode_position(p));
    for (const auto& o : s.orders())
        result["orders"].push_back(decode_order(o));
    Decimal fill_fees;
    for (const auto& f : s.fills()) {
        if (!f.has_fee() || f.fee().units() < 0)
            throw std::invalid_argument("account fill fee is missing or invalid");
        fill_fees = fill_fees + Decimal::from_raw(f.fee().units());
        auto item = instrument_fields(f.venue(), f.symbol());
        item.update(
            {{"id", f.id()},
                {"order_id", f.order_id()},
                {"quantity", get(f.quantity())},
                {"price", get(f.price())},
                {"fee", get(f.fee())}});
        result["fills"].push_back(std::move(item));
    }
    if (fill_fees != Decimal::from_raw(s.fees().units()))
        throw std::invalid_argument("account fill fees differ from ledger total");
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
    if (s.risk_artifact().size() != 64)
        throw std::invalid_argument("invalid risk plugin artifact");
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
        {"risk_artifact", s.risk_artifact()},
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
        {"strategy", nullptr},
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
