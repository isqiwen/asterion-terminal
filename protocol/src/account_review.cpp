#include <asterion/protocol/account_review.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/protocol/trading_facts.hpp>
#include <asterion/foundation/id.hpp>
#include <set>

namespace asterion::protocol {
namespace {

void owner(const resource::proto::Owner& value)
{
    validate_id(value.node_id());
    validate_id(value.service_id());
    if (value.domain() != "research")
        throw std::invalid_argument("account review requires a Research owner");
}

void program(const std::string& value)
{
    if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("account review requires a fixed execution program");
}

Json owner_json(const resource::proto::Owner& value)
{
    return {{"node", value.node_id()}, {"service", value.service_id()}, {"domain", value.domain()}};
}

Json object_json(const resource::proto::Object& value)
{
    auto result = owner_json(value.owner());
    result["kind"] = value.kind();
    result["id"] = value.id();
    result["version"] = value.version();
    return result;
}

} // namespace

void validate_account_review_request(const research::proto::AccountReviewRequest& value)
{
    validate_message(value);
    validate_trading_facts_reference(value.source());
    validate_id(value.account_id());
    if (value.environment() != proto::REAL && value.environment() != proto::BROKER_SIMULATION)
        throw std::invalid_argument("account review requires an explicit trading environment");
}

void validate_account_review_input(const research::proto::AccountReviewInput& value)
{
    validate_message(value);
    owner(value.owner());
    program(value.program_artifact());
    validate_trading_facts_status(value.evidence());
    if (value.evidence().operation().state() != proto::TradingFactsReceipt::PUBLISHED
        || !value.evidence().has_capture())
        throw std::invalid_argument("account review requires complete published fixed evidence");
}

research::proto::AccountReviewInput make_account_review_input(
    const resource::proto::Owner& owner,
    const research::proto::AccountReviewRequest& request,
    const proto::TradingFactsStatus& source,
    const std::string& program_artifact)
{
    validate_account_review_request(request);
    validate_trading_facts_status(source);
    if (source.operation().state() != proto::TradingFactsReceipt::PUBLISHED || !source.has_capture()
        || source.operation().reference().SerializeAsString()
            != request.source().SerializeAsString()
        || source.capture().facts().account_id() != request.account_id()
        || source.capture().facts().environment() != request.environment())
        throw std::invalid_argument("account review source differs from its fixed request");
    research::proto::AccountReviewInput result;
    *result.mutable_owner() = owner;
    *result.mutable_evidence() = source;
    result.set_program_artifact(program_artifact);
    validate_account_review_input(result);
    return result;
}

void validate_account_review_execution(const research::proto::AccountReviewExecution& value)
{
    validate_message(value);
    owner(value.owner());
    program(value.program_artifact());
    validate_account_review_request(value.request());
}

research::proto::AccountReviewExecution account_review_execution(
    const research::proto::AccountReviewInput& input)
{
    validate_account_review_input(input);
    research::proto::AccountReviewExecution result;
    *result.mutable_owner() = input.owner();
    result.set_program_artifact(input.program_artifact());
    auto& request = *result.mutable_request();
    *request.mutable_source() = input.evidence().operation().reference();
    request.set_account_id(input.evidence().capture().facts().account_id());
    request.set_environment(input.evidence().capture().facts().environment());
    return result;
}

void validate_account_review_result(
    const research::proto::AccountReviewInput& input,
    const research::proto::AccountReviewResult& result)
{
    validate_account_review_input(input);
    validate_message(result);
    const auto& facts = input.evidence().capture().facts();
    if (result.owner().SerializeAsString() != input.owner().SerializeAsString()
        || result.source().SerializeAsString()
            != input.evidence().operation().reference().SerializeAsString()
        || result.account_id() != facts.account_id() || result.environment() != facts.environment()
        || result.program_artifact() != input.program_artifact()
        || result.position_records() != static_cast<unsigned>(facts.positions_size())
        || result.order_records() != static_cast<unsigned>(facts.orders_size())
        || result.trade_records() != static_cast<unsigned>(facts.trades_size())
        || result.unconfirmed_orders() != static_cast<unsigned>(facts.unconfirmed_size())
        || result.funds_present() != facts.has_funds()
        || result.positions_reconciled() != facts.positions_reconciled()
        || result.broker_ready() != (facts.phase() == "ready")
        || result.recovery_required() != facts.recovery_required()
        || result.conclusion() != research::proto::AccountReviewResult::EVIDENCE_INSUFFICIENT)
        throw std::invalid_argument("account review result disagrees with fixed evidence");
    const std::set<std::string> known{
        "complete_broker_history",
        "cashflow_history",
        "settlement_evidence",
        "valuation_history",
        "component_business_times",
        "funds_currency",
        "broker_not_ready",
        "known_fills_not_reconciled",
        "funds_observation_missing",
        "source_requires_recovery",
        "unconfirmed_order_intents"
    };
    std::set<std::string> seen;
    for (const auto& gap : result.limitations())
        if (!known.contains(gap) || !seen.insert(gap).second)
            throw std::invalid_argument("invalid account review limitation");
    for (const auto* gap :
        {"complete_broker_history",
            "cashflow_history",
            "settlement_evidence",
            "valuation_history",
            "component_business_times",
            "funds_currency"})
        if (!seen.contains(gap))
            throw std::invalid_argument("account review cannot hide missing historical evidence");
}

Json decode_account_review_input(const research::proto::AccountReviewInput& value)
{
    validate_account_review_input(value);
    return {
        {"owner", owner_json(value.owner())},
        {"program_artifact", value.program_artifact()},
        {"evidence", decode_trading_facts_status(value.evidence())}
    };
}

Json decode_account_review_result(
    const research::proto::AccountReviewInput& input,
    const research::proto::AccountReviewResult& value)
{
    validate_account_review_result(input, value);
    return {
        {"owner", owner_json(value.owner())},
        {"source", object_json(value.source())},
        {"account_id", value.account_id()},
        {"environment", value.environment() == proto::REAL ? "real" : "simulation"},
        {"program_artifact", value.program_artifact()},
        {"conclusion", "evidence_insufficient"},
        {"position_records", value.position_records()},
        {"order_records", value.order_records()},
        {"trade_records", value.trade_records()},
        {"unconfirmed_orders", value.unconfirmed_orders()},
        {"funds_present", value.funds_present()},
        {"positions_reconciled", value.positions_reconciled()},
        {"broker_ready", value.broker_ready()},
        {"recovery_required", value.recovery_required()},
        {"limitations",
            std::vector<std::string>(value.limitations().begin(), value.limitations().end())}
    };
}

} // namespace asterion::protocol
