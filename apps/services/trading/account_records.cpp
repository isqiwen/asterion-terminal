#include "account_records.hpp"
#include <asterion/protocol/trading_facts.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/protocol/trading.hpp>
#include <stdexcept>

namespace asterion::trading {
namespace {

std::string text(const Json& value, const char* key)
{
    auto result = value.at(key).get<std::string>();
    if (result.empty() || result.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid live trading field");
    return result;
}

Offset offset_of(protocol::proto::Offset value)
{
    switch (value) {
        case protocol::proto::OPEN:
            return Offset::open;
        case protocol::proto::CLOSE_TODAY:
            return Offset::close_today;
        case protocol::proto::CLOSE_YESTERDAY:
            return Offset::close_yesterday;
        case protocol::proto::CLOSE:
            return Offset::close;
        default:
            throw std::invalid_argument("invalid open/close offset");
    }
}

const std::string& required_text(const std::string& value)
{
    if (value.empty() || value.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid live trading field");
    return value;
}

bool not_sent_code(int code)
{
    return code == broker_code::session_changed || code == broker_code::exposure_changed
        || code == broker_code::expired || code == broker_code::dispatch_busy
        || code == broker_code::permit_refused;
}

TerminalOrder terminal_order(const Json& record)
{
    require_fields(
        record,
        {"order_id",
            "broker_key",
            "trading_day",
            "status",
            "filled",
            "exchange_order_id",
            "error_code"});
    const auto status = text(record, "status");
    if (!record.at("broker_key").is_string() || !record.at("trading_day").is_string())
        throw std::invalid_argument("invalid persisted terminal order evidence");
    TerminalOrder result{
        text(record, "order_id"),
        record.at("broker_key").get<std::string>(),
        record.at("trading_day").get<std::string>(),
        status == "filled"          ? BrokerOrderStatus::filled
            : status == "cancelled" ? BrokerOrderStatus::cancelled
                                    : BrokerOrderStatus::rejected,
        Decimal::parse(text(record, "filled")),
        {},
        0
    };
    if ((status != "filled" && status != "cancelled" && status != "rejected")
        || result.filled < Decimal{}
        || (result.status == BrokerOrderStatus::rejected && result.filled != Decimal{})
        || !record.at("exchange_order_id").is_string()
        || !record.at("error_code").is_number_integer())
        throw std::invalid_argument("invalid persisted terminal order evidence");
    result.exchange_order_id = record.at("exchange_order_id").get<std::string>();
    result.error_code = record.at("error_code").get<int>();
    return result;
}

JournalEntry command_body(const AccountRequest& request, std::string_view policy_revision)
{
    JournalEntry result;
    result.body = {{"command", request.command}, {"policy_revision", policy_revision}};
    result.command_id = request.id;
    result.trace =
        {request.id, std::string(request.action()), std::string(request.order_id()), {}, {}};
    return result;
}

} // namespace

Json account_manifest(const Json& header)
{
    require_fields(header, {"risk_artifact", "manifest", "account_id", "policy_revision"});
    validate_id(text(header, "account_id"));
    validate_id(text(header, "policy_revision"));
    auto manifest = protocol::decode_live_input(protocol::encode_live_input(header.at("manifest")));
    if (manifest.at("account_id") != header.at("account_id"))
        throw std::invalid_argument("live account identity differs from record header");
    return manifest;
}

std::string_view status_name(BrokerOrderStatus status) noexcept
{
    switch (status) {
        case BrokerOrderStatus::submitted:
            return "submitted";
        case BrokerOrderStatus::accepted:
            return "accepted";
        case BrokerOrderStatus::partially_filled:
            return "partially_filled";
        case BrokerOrderStatus::filled:
            return "filled";
        case BrokerOrderStatus::cancelled:
            return "cancelled";
        case BrokerOrderStatus::rejected:
            return "rejected";
    }
    return "submitted";
}

AccountRequest AccountRequest::from_wire(const protocol::proto::Command& command)
{
    AccountRequest result;
    result.id = command.request_id();
    validate_id(result.id);
    // The current durable representation is derived once at the ingress boundary.
    // It is never parsed to obtain the typed operation below.
    result.command = protocol::decode_command(command);
    switch (command.operation_case()) {
        case protocol::proto::Command::kSubmit: {
            const auto& order = command.submit();
            validate_id(order.order_id());
            result.operation = SubmitOrder{
                {order.order_id(),
                    {order.venue(), order.symbol()},
                    order.side() == protocol::proto::BUY ? Side::buy : Side::sell,
                    Decimal::from_raw(order.quantity().units()),
                    Decimal::from_raw(order.price().units())},
                offset_of(order.offset())
            };
            break;
        }
        case protocol::proto::Command::kCancel:
            result.operation = CancelOrder{required_text(command.cancel().order_id())};
            break;
        case protocol::proto::Command::kLiveAuthorize:
            result.operation = Authorize{command.live_authorize().user_id()};
            break;
        case protocol::proto::Command::kLiveRevoke:
            result.operation = Revoke{};
            break;
        case protocol::proto::Command::kLiveResolve:
            result.operation = ResolveOrder{required_text(command.live_resolve().order_id())};
            break;
        case protocol::proto::Command::kLivePolicy:
            result.operation = ChangePolicy{
                PolicyDefinition::from_wire(command.live_policy().policy()),
                required_text(command.live_policy().risk_artifact())
            };
            break;
        case protocol::proto::Command::kStrategyStart: {
            const auto& start = command.strategy_start();
            if (result.id.size() > 64)
                throw std::invalid_argument("strategy run identity is too long");
            validate_id(start.market_service());
            result.operation = StartStrategy{
                {start.venue(), start.symbol()},
                start.strategy(),
                required_text(start.market_endpoint()),
                start.market_service()
            };
            break;
        }
        case protocol::proto::Command::kStrategyStop:
            result.operation = StopStrategy{};
            break;
        default:
            throw std::invalid_argument("unsupported live trading operation");
    }
    return result;
}

AccountRequest AccountRequest::from_record(const Json& command)
{
    auto result = from_wire(protocol::encode_command(command));
    // Preserve recorded content exactly for request identity comparisons.
    result.command = command;
    return result;
}

std::string_view AccountRequest::action() const noexcept
{
    struct Name
    {
        std::string_view operator()(const SubmitOrder&) const
        {
            return "submit";
        }

        std::string_view operator()(const CancelOrder&) const
        {
            return "cancel";
        }

        std::string_view operator()(const Authorize&) const
        {
            return "live_authorize";
        }

        std::string_view operator()(const Revoke&) const
        {
            return "live_revoke";
        }

        std::string_view operator()(const ResolveOrder&) const
        {
            return "live_resolve";
        }

        std::string_view operator()(const ChangePolicy&) const
        {
            return "live_policy";
        }

        std::string_view operator()(const StartStrategy&) const
        {
            return "strategy_start";
        }

        std::string_view operator()(const StopStrategy&) const
        {
            return "strategy_stop";
        }
    };

    return std::visit(Name{}, operation);
}

std::string_view AccountRequest::order_id() const noexcept
{
    if (const auto* submit = as<SubmitOrder>())
        return submit->order.id;
    if (const auto* cancel = as<CancelOrder>())
        return cancel->order_id;
    if (const auto* resolve = as<ResolveOrder>())
        return resolve->order_id;
    return {};
}

Json Authorization::json() const
{
    return {{"authorized_at_ms", authorized_at_ms}};
}

JournalRecord parse_record(const Json& record)
{
    if (record.is_object() && record.contains("facts_decision")) {
        require_fields(record, {"facts_decision"});
        const auto& f = record.at("facts_decision");
        require_fields(
            f,
            {"node", "service", "id", "version", "state", "admitted_at_ms", "published_at_ms"});
        protocol::proto::TradingFactsReceipt receipt;
        auto& ref = *receipt.mutable_reference();
        ref.mutable_owner()->set_node_id(text(f, "node"));
        ref.mutable_owner()->set_service_id(text(f, "service"));
        ref.mutable_owner()->set_domain("trading");
        ref.set_kind("trading_facts");
        ref.set_id(text(f, "id"));
        ref.set_version(text(f, "version"));
        const auto state = text(f, "state");
        if (state != "admitted" && state != "published")
            throw std::invalid_argument("invalid trading facts decision");
        if (!f.at("admitted_at_ms").is_number_integer()
            || !f.at("published_at_ms").is_number_integer())
            throw std::invalid_argument("invalid trading facts decision times");
        receipt.set_state(
            state == "admitted" ? protocol::proto::TradingFactsReceipt::ADMITTED
                                : protocol::proto::TradingFactsReceipt::PUBLISHED);
        receipt.set_admitted_at_ms(f.at("admitted_at_ms").get<std::int64_t>());
        receipt.set_published_at_ms(f.at("published_at_ms").get<std::int64_t>());
        protocol::validate_trading_facts_receipt(receipt);
        return FactsDecision{std::move(receipt)};
    }
    if (record.is_object() && record.contains("orders_terminal")) {
        require_fields(record, {"orders_terminal"});
        const auto& orders = record.at("orders_terminal");
        if (!orders.is_array() || orders.empty() || orders.size() > OrdersTerminal::batch)
            throw std::invalid_argument("invalid persisted terminal order evidence");
        OrdersTerminal result;
        for (const auto& order : orders)
            result.orders.push_back(terminal_order(order));
        return result;
    }
    if (record.is_object() && record.contains("order_not_sent")) {
        require_fields(record, {"order_not_sent", "error_code"});
        if (!record.at("error_code").is_number_integer()
            || !not_sent_code(record.at("error_code").get<int>()))
            throw std::invalid_argument("invalid unsent live order result");
        return OrderNotSent{text(record, "order_not_sent"), record.at("error_code").get<int>()};
    }
    if (record.is_object() && record.contains("cancel_result")) {
        require_fields(record, {"cancel_result", "outcome"});
        const auto outcome = text(record, "outcome");
        if (outcome != "dispatch_acknowledged" && outcome != "unknown")
            throw std::invalid_argument("invalid live cancellation result");
        return CancelResult{text(record, "cancel_result"), outcome == "dispatch_acknowledged"};
    }
    if (!record.is_object() || !record.contains("command"))
        throw std::invalid_argument("invalid live trading record");
    const auto& command = record.at("command");
    CommandRecord result{
        AccountRequest::from_record(command),
        text(record, "policy_revision"),
        {},
        {},
        {},
        {}
    };
    if (const auto* policy = result.request.as<ChangePolicy>()) {
        require_fields(
            record,
            {"command", "policy_revision", "new_policy_revision", "risk_artifact"});
        result.new_policy_revision = text(record, "new_policy_revision");
        validate_id(result.new_policy_revision);
        result.risk_artifact = text(record, "risk_artifact");
        if (policy->risk_artifact != result.risk_artifact)
            throw std::invalid_argument("policy algorithm does not match its recorded artifact");
    } else if (result.request.as<Authorize>())
        require_fields(record, {"command", "policy_revision", "authorization"});
    else if (result.request.as<SubmitOrder>()) {
        require_fields(record, {"command", "policy_revision", "broker_key", "trading_day"});
        result.broker_key = text(record, "broker_key");
        result.trading_day = text(record, "trading_day");
    } else
        require_fields(record, {"command", "policy_revision"});
    return result;
}

JournalEntry command_entry(const AccountRequest& request, std::string_view policy_revision)
{
    auto result = command_body(request, policy_revision);
    if (const auto* resolve = request.as<ResolveOrder>())
        result.excluded_order = resolve->order_id;
    return result;
}

JournalEntry submission_entry(
    const AccountRequest& request,
    std::string_view policy_revision,
    std::string_view broker_key,
    std::string_view trading_day)
{
    auto result = command_body(request, policy_revision);
    result.body["broker_key"] = broker_key;
    result.body["trading_day"] = trading_day;
    result.order_id = request.order_id();
    result.trace.broker_key = broker_key;
    return result;
}

JournalEntry policy_entry(
    const AccountRequest& request,
    std::string_view policy_revision,
    std::string_view new_policy_revision,
    std::string_view risk_artifact)
{
    auto result = command_body(request, policy_revision);
    result.body["new_policy_revision"] = new_policy_revision;
    result.body["risk_artifact"] = risk_artifact;
    return result;
}

JournalEntry authorization_entry(
    const AccountRequest& request,
    std::string_view policy_revision,
    const Authorization& authorization)
{
    auto result = command_body(request, policy_revision);
    result.body["authorization"] = authorization.json();
    return result;
}

JournalEntry entry(const OrdersTerminal& record)
{
    Json orders = Json::array();
    for (const auto& order : record.orders)
        orders.push_back(
            {{"order_id", order.order_id},
                {"broker_key", order.broker_key},
                {"trading_day", order.trading_day},
                {"status", status_name(order.status)},
                {"filled", order.filled.str()},
                {"exchange_order_id", order.exchange_order_id},
                {"error_code", order.error_code}});
    JournalEntry result;
    result.body = {{"orders_terminal", std::move(orders)}};
    return result;
}

JournalEntry entry(const OrderNotSent& record)
{
    JournalEntry result;
    result.body = {{"order_not_sent", record.order_id}, {"error_code", record.error_code}};
    result.excluded_order = record.order_id;
    result.trace.order_id = record.order_id;
    result.trace.outcome = "not_sent";
    return result;
}

JournalEntry entry(const CancelResult& record)
{
    JournalEntry result;
    result.trace.request_id = record.request_id;
    result.trace.outcome = record.acknowledged ? "dispatch_acknowledged" : "unknown";
    result.body = {{"cancel_result", record.request_id}, {"outcome", result.trace.outcome}};
    return result;
}

std::string
facts_decision_key(std::string_view id, protocol::proto::TradingFactsReceipt::State state)
{
    validate_id(id);
    if (state != protocol::proto::TradingFactsReceipt::ADMITTED
        && state != protocol::proto::TradingFactsReceipt::PUBLISHED)
        throw std::invalid_argument("invalid trading facts decision state");
    return "@facts/" + std::string(id)
        + (state == protocol::proto::TradingFactsReceipt::ADMITTED ? "/admitted" : "/published");
}

JournalEntry entry(const FactsDecision& decision)
{
    const auto& r = decision.receipt;
    protocol::validate_trading_facts_receipt(r);
    const auto& ref = r.reference();
    const auto state =
        r.state() == protocol::proto::TradingFactsReceipt::ADMITTED ? "admitted" : "published";
    JournalEntry result;
    result.command_id = facts_decision_key(ref.id(), r.state());
    result.trace.request_id = ref.id();
    result.trace.action = std::string("facts.") + state;
    result.body = {
        {"facts_decision",
            {{"node", ref.owner().node_id()},
                {"service", ref.owner().service_id()},
                {"id", ref.id()},
                {"version", ref.version()},
                {"state", state},
                {"admitted_at_ms", r.admitted_at_ms()},
                {"published_at_ms", r.published_at_ms()}}}
    };
    return result;
}

} // namespace asterion::trading
