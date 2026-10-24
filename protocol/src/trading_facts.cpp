#include <asterion/protocol/trading_facts.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/foundation/decimal.hpp>
#include <set>

namespace asterion::protocol {
namespace {

std::string decimal(const proto::Decimal& value)
{
    return Decimal::from_raw(value.units()).str();
}

void instrument(std::string_view venue, std::string_view symbol)
{
    validate_id(venue);
    validate_id(symbol);
}

void side(proto::Side value)
{
    if (value != proto::BUY && value != proto::SELL)
        throw std::invalid_argument("invalid trading facts side");
}

void offset(proto::Offset value)
{
    if (value != proto::OPEN && value != proto::CLOSE && value != proto::CLOSE_TODAY
        && value != proto::CLOSE_YESTERDAY)
        throw std::invalid_argument("invalid trading facts offset");
}

} // namespace

void validate_trading_facts_reference(const resource::proto::Object& ref)
{
    validate_message(ref);
    validate_id(ref.owner().node_id());
    validate_id(ref.owner().service_id());
    validate_id(ref.id());
    if (ref.owner().domain() != "trading" || ref.kind() != "trading_facts"
        || ref.version().size() != 64
        || ref.version().find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid trading facts identity");
}

void validate_trading_facts(const proto::TradingFactsCapture& capture)
{
    validate_message(capture);
    validate_trading_facts_reference(capture.reference());
    const auto& f = capture.facts();
    validate_id(f.instance());
    validate_id(f.id());
    validate_id(f.account_id());
    validate_id(f.policy_revision());
    if (f.risk_artifact().size() != 64
        || f.risk_artifact().find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid trading facts risk artifact");
    const std::set<std::string> phases{
        "disconnected",
        "connecting",
        "authenticating",
        "logging_in",
        "confirming",
        "synchronizing",
        "ready",
        "error"
    };
    if (capture.ByteSizeLong() > max_trading_facts_bytes || f.captured_at_ms() <= 0
        || f.synchronized_ms() < 0 || !phases.contains(f.phase())
        || (f.environment() != proto::REAL && f.environment() != proto::BROKER_SIMULATION)
        || f.source().SerializeAsString() != capture.reference().owner().SerializeAsString()
        || f.id() != capture.reference().id()
        || sha256_bytes(f.SerializeAsString()) != capture.reference().version())
        throw std::invalid_argument("invalid trading facts evidence");
    if (f.positions_size() > 4096 || f.orders_size() > 4096 || f.trades_size() > 4096
        || f.unconfirmed_size() > 4096)
        throw std::invalid_argument("trading facts observation is too large");
    for (const auto& p : f.positions()) {
        instrument(p.venue(), p.symbol());
        side(p.side());
        if (p.today().units() < 0 || p.yesterday().units() < 0)
            throw std::invalid_argument("invalid trading facts position");
    }
    const std::set<std::string>
        statuses{"submitted", "accepted", "partially_filled", "filled", "cancelled", "rejected"};
    for (const auto& o : f.orders()) {
        instrument(o.venue(), o.symbol());
        side(o.side());
        offset(o.offset());
        if (o.quantity().units() <= 0 || o.filled().units() < 0
            || o.filled().units() > o.quantity().units() || o.limit_price().units() <= 0
            || !statuses.contains(o.status()))
            throw std::invalid_argument("invalid trading facts order");
    }
    for (const auto& t : f.trades()) {
        instrument(t.venue(), t.symbol());
        side(t.side());
        offset(t.offset());
        if (t.quantity().units() <= 0 || t.price().units() <= 0)
            throw std::invalid_argument("invalid trading facts trade");
    }
}

proto::TradingFactsCapture freeze_trading_facts(
    const resource::proto::Owner& owner,
    const std::string& instance,
    const std::string& id,
    std::int64_t captured_at_ms,
    const proto::LiveSnapshot& snapshot)
{
    (void)decode_live_snapshot(snapshot);
    proto::TradingFactsCapture result;
    auto& f = *result.mutable_facts();
    *f.mutable_source() = owner;
    f.set_instance(instance);
    f.set_id(id);
    f.set_captured_at_ms(captured_at_ms);
    f.set_account_id(snapshot.account_id());
    f.set_environment(snapshot.environment());
    f.set_policy_revision(snapshot.policy_revision());
    f.set_risk_artifact(snapshot.risk_artifact());
    f.set_broker_sequence(snapshot.broker_sequence());
    f.set_connection_generation(snapshot.connection_generation());
    f.set_synchronized_ms(snapshot.synchronized_ms());
    f.set_phase(snapshot.phase());
    f.set_error_code(snapshot.error_code());
    f.set_trading_day(snapshot.trading_day());
    f.set_positions_reconciled(snapshot.positions_reconciled());
    f.set_recovery_required(snapshot.recovery_required());
    if (snapshot.has_funds())
        *f.mutable_funds() = snapshot.funds();
    *f.mutable_positions() = snapshot.positions();
    *f.mutable_orders() = snapshot.orders();
    *f.mutable_trades() = snapshot.trades();
    *f.mutable_unconfirmed() = snapshot.unconfirmed();
    auto& ref = *result.mutable_reference();
    *ref.mutable_owner() = owner;
    ref.set_kind("trading_facts");
    ref.set_id(id);
    ref.set_version(sha256_bytes(f.SerializeAsString()));
    validate_trading_facts(result);
    return result;
}

namespace {

std::string offset_name(proto::Offset value)
{
    offset(value);
    switch (value) {
        case proto::OPEN:
            return "open";
        case proto::CLOSE_TODAY:
            return "close_today";
        case proto::CLOSE_YESTERDAY:
            return "close_yesterday";
        default:
            return "close";
    }
}

} // namespace

Json decode_trading_facts(const proto::TradingFactsCapture& capture)
{
    validate_trading_facts(capture);
    const auto& f = capture.facts();
    const auto& ref = capture.reference();
    Json result{
        {"reference",
            {{"node", ref.owner().node_id()},
                {"service", ref.owner().service_id()},
                {"domain", "trading"},
                {"kind", ref.kind()},
                {"id", ref.id()},
                {"version", ref.version()}}},
        {"account_id", f.account_id()},
        {"environment", f.environment() == proto::REAL ? "real" : "simulation"},
        {"instance", f.instance()},
        {"captured_at_ms", std::to_string(f.captured_at_ms())},
        {"broker_sequence", std::to_string(f.broker_sequence())},
        {"connection_generation", std::to_string(f.connection_generation())},
        {"synchronized_ms", std::to_string(f.synchronized_ms())},
        {"phase", f.phase()},
        {"error_code", f.error_code()},
        {"trading_day", f.trading_day()},
        {"positions_reconciled", f.positions_reconciled()},
        {"recovery_required", f.recovery_required()},
        {"policy_revision", f.policy_revision()},
        {"risk_artifact", f.risk_artifact()},
        {"complete_history", false},
        {"atomic_ledger_cut", false},
        {"funds", nullptr},
        {"positions", Json::array()},
        {"orders", Json::array()},
        {"trades", Json::array()},
        {"unconfirmed", Json::array()},
        {"missing_evidence",
            {"cashflow_history",
                "settlement_evidence",
                "valuation_history",
                "component_business_times",
                "complete_broker_history",
                "funds_currency"}}
    };
    if (f.has_funds()) {
        const auto& v = f.funds();
        result["funds"] = {
            {"balance", decimal(v.balance())},
            {"available", decimal(v.available())},
            {"margin", decimal(v.margin())},
            {"commission", decimal(v.commission())},
            {"close_profit", decimal(v.close_profit())},
            {"position_profit", decimal(v.position_profit())}
        };
    }
    for (const auto& p : f.positions())
        result["positions"].push_back(
            {{"venue", p.venue()},
                {"symbol", p.symbol()},
                {"side", p.side() == proto::BUY ? "buy" : "sell"},
                {"today", decimal(p.today())},
                {"yesterday", decimal(p.yesterday())}});
    for (const auto& o : f.orders())
        result["orders"].push_back(
            {{"id", o.id()},
                {"venue", o.venue()},
                {"symbol", o.symbol()},
                {"broker_key", o.broker_key()},
                {"exchange_order_id", o.exchange_order_id()},
                {"side", o.side() == proto::BUY ? "buy" : "sell"},
                {"offset", offset_name(o.offset())},
                {"quantity", decimal(o.quantity())},
                {"filled", decimal(o.filled())},
                {"limit_price", decimal(o.limit_price())},
                {"status", o.status()},
                {"error_code", o.error_code()}});
    for (const auto& t : f.trades())
        result["trades"].push_back(
            {{"id", t.id()},
                {"order_id", t.order_id()},
                {"venue", t.venue()},
                {"symbol", t.symbol()},
                {"side", t.side() == proto::BUY ? "buy" : "sell"},
                {"offset", offset_name(t.offset())},
                {"quantity", decimal(t.quantity())},
                {"price", decimal(t.price())},
                {"trading_day", t.trading_day()},
                {"trade_time", t.trade_time()}});
    for (const auto& u : f.unconfirmed())
        result["unconfirmed"].push_back(
            {{"id", u.id()}, {"broker_key", u.broker_key()}, {"trading_day", u.trading_day()}});
    return result;
}

void validate_trading_facts_receipt(const proto::TradingFactsReceipt& r)
{
    validate_message(r);
    validate_trading_facts_reference(r.reference());
    if (r.admitted_at_ms() <= 0
        || (r.state() != proto::TradingFactsReceipt::ADMITTED
            && r.state() != proto::TradingFactsReceipt::PUBLISHED)
        || (r.state() == proto::TradingFactsReceipt::ADMITTED && r.published_at_ms() != 0)
        || (r.state() == proto::TradingFactsReceipt::PUBLISHED && r.published_at_ms() <= 0))
        throw std::invalid_argument("invalid trading facts receipt");
}

namespace {

void validate_status_envelope(const proto::TradingFactsStatus& value)
{
    validate_message(value);
    validate_trading_facts_receipt(value.operation());
    if (value.has_capture()
        && (value.operation().state() != proto::TradingFactsReceipt::PUBLISHED
            || value.capture().reference().SerializeAsString()
                != value.operation().reference().SerializeAsString()))
        throw std::invalid_argument("trading facts payload is not confirmed by its receipt");
}

} // namespace

void validate_trading_facts_status(const proto::TradingFactsStatus& value)
{
    validate_status_envelope(value);
    if (value.has_capture())
        validate_trading_facts(value.capture());
}

Json decode_trading_facts_status(const proto::TradingFactsStatus& value)
{
    validate_status_envelope(value);
    const auto& r = value.operation();
    const auto& ref = r.reference();
    Json result{
        {"id", ref.id()},
        {"state", r.state() == proto::TradingFactsReceipt::PUBLISHED ? "published" : "admitted"},
        {"admitted_at_ms", std::to_string(r.admitted_at_ms())},
        {"reference",
            {{"node", ref.owner().node_id()},
                {"service", ref.owner().service_id()},
                {"domain", "trading"},
                {"kind", ref.kind()},
                {"id", ref.id()},
                {"version", ref.version()}}}
    };
    if (r.state() == proto::TradingFactsReceipt::PUBLISHED)
        result["published_at_ms"] = std::to_string(r.published_at_ms());
    if (value.has_capture()) {
        if (r.state() != proto::TradingFactsReceipt::PUBLISHED
            || value.capture().reference().SerializeAsString() != ref.SerializeAsString())
            throw std::invalid_argument("trading facts payload is not confirmed by its receipt");
        result["capture"] = decode_trading_facts(value.capture());
    }
    return result;
}

} // namespace asterion::protocol
