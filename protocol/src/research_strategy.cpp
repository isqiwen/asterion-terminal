#include <set>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/protocol/research_logic.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <cctype>
#include <algorithm>

namespace asterion::protocol {
namespace {
namespace r = research::proto;

bool hash(const std::string& s)
{
    return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

void owner(const resource::proto::Owner& value)
{
    validate_message(value);
    validate_id(value.node_id());
    validate_id(value.service_id());
    if (value.domain() != "research")
        throw std::invalid_argument("research strategy owner required");
}

bool text(const std::string& s, std::size_t max)
{
    return !s.empty() && s.size() <= max && s.find(char(0)) == std::string::npos
        && !std::ranges::all_of(s, [](unsigned char c) {
               return std::isspace(c);
           });
}

resource::proto::Object object(
    const resource::proto::Owner& who,
    const std::string& id,
    const std::string& digest,
    const char* kind)
{
    resource::proto::Object value;
    *value.mutable_owner() = who;
    value.set_id(id);
    value.set_version(digest);
    value.set_kind(kind);
    return value;
}

} // namespace

void validate_strategy_object(const resource::proto::Object& value, std::string_view kind)
{
    validate_message(value);
    owner(value.owner());
    validate_id(value.id());
    if (value.kind() != kind || !hash(value.version()))
        throw std::invalid_argument("invalid research strategy identity");
}

void validate_strategy_definition(const r::StrategyDefinition& value)
{
    validate_message(value);
    if (value.interval_minutes() > 1440 || !hash(value.program_artifact())
        || !hash(value.risk_artifact()))
        throw std::invalid_argument(
            "strategy candidate requires one ordinary contract and one fixed CTA rule");
    if (value.has_portfolio()) {
        const auto& portfolio = value.portfolio();
        if (value.has_contract() || value.interval_minutes() != 0 || portfolio.contracts_size() < 2
            || portfolio.contracts_size() > 20 || !portfolio.has_strategy()
            || !portfolio.strategy().has_fixed_weights()
            || portfolio.strategy().fixed_weights().members_size() != portfolio.contracts_size())
            throw std::invalid_argument(
                "portfolio candidate requires its complete ordinary daily contracts and fixed-weight policy");
        std::set<std::pair<std::string, std::string>> members;
        for (const auto& member : portfolio.strategy().fixed_weights().members())
            members.emplace(member.venue(), member.symbol());
        const auto& currency = portfolio.contracts(0).currency();
        for (const auto& contract : portfolio.contracts()) {
            (void)decode_contract(contract);
            if (contract.currency() != currency
                || !members.erase({contract.venue(), contract.symbol()}))
                throw std::invalid_argument(
                    "portfolio candidate contracts differ from its declared policy");
            validate_strategy(
                portfolio.strategy(),
                Decimal::from_raw(contract.quantity_increment().units()));
        }
        if (!members.empty())
            throw std::invalid_argument(
                "portfolio candidate contracts differ from its declared policy");
        return;
    }
    (void)decode_contract(value.contract());
    if (!value.has_strategy() || value.strategy().has_cross()
        || value.strategy().has_fixed_weights())
        throw std::invalid_argument(
            "strategy candidate requires exactly one ordinary decision definition");
    validate_strategy(
        value.strategy(),
        Decimal::from_raw(value.contract().quantity_increment().units()));
}

r::StrategyDefinition strategy_definition(
    const backtest::proto::BacktestInput& input,
    const std::string& programme,
    const std::string& risk)
{
    validate_message(input);
    const bool portfolio = input.strategies_size() == 1 && input.strategies(0).has_fixed_weights();
    if ((!portfolio && input.paper().contracts_size() != 1)
        || (portfolio && input.paper().contracts_size() < 2) || input.strategies_size() != 1
        || input.series_size() || input.has_walk_forward() || !input.holdout_day().empty())
        throw std::invalid_argument(
            "strategy candidate requires one ordinary contract and one fixed CTA rule");
    r::StrategyDefinition value;
    if (portfolio) {
        auto& fixed = *value.mutable_portfolio();
        *fixed.mutable_strategy() = input.strategies(0);
        for (const auto& member : input.paper().contracts()) {
            if (member.dataset().interval_minutes() != 0)
                throw std::invalid_argument("portfolio candidate requires ordinary daily data");
            *fixed.add_contracts() = member.dataset().contract();
        }
    } else {
        *value.mutable_strategy() = input.strategies(0);
        *value.mutable_contract() = input.paper().contracts(0).dataset().contract();
    }
    value.set_interval_minutes(input.paper().contracts(0).dataset().interval_minutes());
    value.set_program_artifact(programme);
    value.set_risk_artifact(risk);
    validate_strategy_definition(value);
    return value;
}

std::string strategy_digest(const r::StrategyDefinition& value)
{
    return sha256_bytes(value.SerializeAsString());
}

const resource::proto::Object& strategy_source(const r::StrategyPackage& value)
{
    return value.has_logic() ? value.logic().result() : value.reference_result();
}

const resource::proto::Object& strategy_source(const r::StrategyDelivery& value)
{
    return value.has_logic_trial() ? value.logic_trial() : value.reference_result();
}

void validate_strategy_logic(
    const r::LogicDefinition& logic,
    const r::StrategyDefinition& definition)
{
    validate_logic_definition(logic);
    validate_strategy_definition(definition);
    if (!definition.strategy().has_moving_average()
        || definition.strategy().moving_average().fast() != logic.fast()
        || definition.strategy().moving_average().slow() != logic.slow()
        || (definition.interval_minutes() != 1 && definition.interval_minutes() != 0))
        throw std::invalid_argument("strategy differs from its fixed logic definition");
}

void validate_strategy_package(const r::StrategyPackage& value)
{
    validate_message(value);
    owner(value.owner());
    validate_id(value.id());
    const auto& source = strategy_source(value);
    validate_strategy_object(source, value.has_logic() ? "logic_trial_result" : "backtest_result");
    validate_strategy_definition(value.definition());
    if (value.has_logic()) {
        const auto& evidence = value.logic();
        validate_strategy_logic(evidence.definition(), value.definition());
        validate_logic_object(evidence.example(), "logic_case");
        if (evidence.definition().owner().SerializeAsString() != value.owner().SerializeAsString()
            || evidence.example().owner().SerializeAsString() != value.owner().SerializeAsString())
            throw std::invalid_argument("strategy logic evidence owner mismatch");
    }
    if (source.owner().SerializeAsString() != value.owner().SerializeAsString()
        || !text(value.name(), 256) || !text(value.purpose(), 4096)
        || !text(value.limitations(), 4096) || !hash(value.source_input_sha256())
        || !value.source_attempt() || value.recorded_at_ms() < 0 || value.ByteSizeLong() > 32768
        || (!value.retirement_reason().empty() && !text(value.retirement_reason(), 4096))
        || value.digest() != strategy_digest(value.definition()))
        throw std::invalid_argument("invalid research strategy package");
}

std::string strategy_delivery_digest(r::StrategyDelivery value)
{
    value.clear_digest();
    value.clear_recorded_at_ms();
    return sha256_bytes(value.SerializeAsString());
}

void validate_strategy_delivery(const r::StrategyDelivery& value)
{
    validate_message(value);
    owner(value.owner());
    validate_id(value.id());
    validate_id(value.target_task_id());
    validate_strategy_object(value.package(), "strategy_package");
    const auto& source = strategy_source(value);
    validate_strategy_object(
        source,
        value.has_logic_trial() ? "logic_trial_result" : "backtest_result");
    if (value.package().owner().SerializeAsString() != value.owner().SerializeAsString()
        || source.owner().SerializeAsString() != value.owner().SerializeAsString()
        || !hash(value.source_input_sha256()) || value.target_task_id() == source.id()
        || value.recorded_at_ms() < 0 || value.ByteSizeLong() > 8192
        || value.digest() != strategy_delivery_digest(value))
        throw std::invalid_argument("invalid research strategy delivery");
}

void validate_strategy_use(const r::StrategyUse& value)
{
    validate_message(value);
    validate_strategy_object(value.package(), "strategy_package");
    validate_strategy_object(value.delivery(), "strategy_delivery");
    if (value.package().owner().SerializeAsString() != value.delivery().owner().SerializeAsString()
        || (!value.request_sha256().empty() && !hash(value.request_sha256())))
        throw std::invalid_argument("invalid research strategy use");
}

resource::proto::Object strategy_package_object(const r::StrategyPackage& value)
{
    validate_strategy_package(value);
    return object(value.owner(), value.id(), value.digest(), "strategy_package");
}

resource::proto::Object strategy_delivery_object(const r::StrategyDelivery& value)
{
    validate_strategy_delivery(value);
    return object(value.owner(), value.id(), value.digest(), "strategy_delivery");
}

Json decode_strategy_object(const resource::proto::Object& value)
{
    validate_strategy_object(value, value.kind());
    return {
        {"node", value.owner().node_id()},
        {"service", value.owner().service_id()},
        {"domain", value.owner().domain()},
        {"kind", value.kind()},
        {"id", value.id()},
        {"version", value.version()}
    };
}

resource::proto::Object encode_strategy_object(const Json& value, std::string_view kind)
{
    require_fields(value, {"node", "service", "domain", "kind", "id", "version"});
    resource::proto::Object result;
    result.mutable_owner()->set_node_id(value.at("node").get<std::string>());
    result.mutable_owner()->set_service_id(value.at("service").get<std::string>());
    result.mutable_owner()->set_domain(value.at("domain").get<std::string>());
    result.set_kind(value.at("kind").get<std::string>());
    result.set_id(value.at("id").get<std::string>());
    result.set_version(value.at("version").get<std::string>());
    validate_strategy_object(result, kind);
    return result;
}

Json decode_strategy_package(const r::StrategyPackage& value)
{
    validate_strategy_package(value);
    const auto& d = value.definition();
    Json definition = {
        {"interval_minutes", d.interval_minutes()},
        {"program_artifact", d.program_artifact()},
        {"risk_artifact", d.risk_artifact()}
    };
    if (d.has_portfolio()) {
        Json contracts = Json::array();
        for (const auto& contract : d.portfolio().contracts())
            contracts.push_back(decode_contract(contract));
        definition["portfolio"] = {
            {"contracts", std::move(contracts)},
            {"strategy", decode_strategy(d.portfolio().strategy())}
        };
    } else {
        definition["contract"] = decode_contract(d.contract());
        definition["strategy"] = decode_strategy(d.strategy());
    }
    return {
        {"object", decode_strategy_object(strategy_package_object(value))},
        {"name", value.name()},
        {"purpose", value.purpose()},
        {"limitations", value.limitations()},
        {"definition", std::move(definition)},
        {"reference_result", value.has_reference_result()?decode_strategy_object(value.reference_result()):Json(nullptr)},
        {"logic", value.has_logic()?Json{{"definition",decode_logic_definition(value.logic().definition())},{"example",decode_logic_object(value.logic().example())},{"result",decode_logic_object(value.logic().result())}}:Json(nullptr)},
        {"evidence_relation", value.has_logic()?"logic_trial":"reference_only"},
        {"source_input_sha256", value.source_input_sha256()},
        {"source_attempt", value.source_attempt()},
        {"recorded_at_ms", std::to_string(value.recorded_at_ms())},
        {"retirement_reason", value.retirement_reason()}
    };
}

Json decode_strategy_delivery(const r::StrategyDelivery& value)
{
    validate_strategy_delivery(value);
    return {
        {"object", decode_strategy_object(strategy_delivery_object(value))},
        {"package", decode_strategy_object(value.package())},
        {"reference_result",
            value.has_reference_result() ? decode_strategy_object(value.reference_result())
                                         : Json(nullptr)},
        {"logic_trial",
            value.has_logic_trial() ? decode_logic_object(value.logic_trial()) : Json(nullptr)},
        {"target_task_id", value.target_task_id()},
        {"source_input_sha256", value.source_input_sha256()},
        {"recorded_at_ms", std::to_string(value.recorded_at_ms())}
    };
}

research::proto::StrategyUse encode_strategy_use(const Json& value)
{
    if (value.contains("request_sha256"))
        require_fields(value, {"package", "delivery", "request_sha256"});
    else
        require_fields(value, {"package", "delivery"});
    research::proto::StrategyUse result;
    *result.mutable_package() = encode_strategy_object(value.at("package"), "strategy_package");
    *result.mutable_delivery() = encode_strategy_object(value.at("delivery"), "strategy_delivery");
    if (value.contains("request_sha256"))
        result.set_request_sha256(value.at("request_sha256").get<std::string>());
    validate_strategy_use(result);
    return result;
}

Json decode_strategy_use(const research::proto::StrategyUse& value)
{
    validate_strategy_use(value);
    Json result = {
        {"package", decode_strategy_object(value.package())},
        {"delivery", decode_strategy_object(value.delivery())}
    };
    if (!value.request_sha256().empty())
        result["request_sha256"] = value.request_sha256();
    return result;
}

Json decode_strategy_reply(
    const r::StrategyReply& reply,
    const r::StrategyRequest& request,
    const resource::proto::Owner& expected)
{
    validate_message(reply);
    validate_message(request);
    owner(expected);
    const auto reject = [] {
        throw std::invalid_argument("invalid research strategy response");
    };
    const auto check_owner = [&](const resource::proto::Owner& actual) {
        if (actual.SerializeAsString() != expected.SerializeAsString())
            throw std::invalid_argument("research strategy response owner mismatch");
    };
    const bool package_wanted = request.has_get()
        || (request.has_mutation()
            && (request.mutation().has_freeze() || request.mutation().has_logic()
                || request.mutation().has_retire()));
    const bool delivery_wanted =
        request.has_delivery() || (request.has_mutation() && request.mutation().has_deliver());
    if (reply.has_package() != package_wanted || reply.has_delivery() != delivery_wanted
        || (!request.has_delivery() && reply.target_accepted()))
        reject();
    Json result = {
        {"packages", Json::array()},
        {"deliveries", Json::array()},
        {"next_after", reply.next_after()}
    };
    if (package_wanted || delivery_wanted) {
        if (reply.packages_size() || reply.deliveries_size() || !reply.next_after().empty())
            reject();
        if (package_wanted) {
            const auto& value = reply.package();
            check_owner(value.owner());
            if (request.has_get()) {
                if (value.id() != request.get().id())
                    reject();
            } else if (request.mutation().has_freeze()) {
                const auto& input = request.mutation().freeze();
                if (value.id() != input.id() || value.name() != input.name()
                    || value.purpose() != input.purpose()
                    || value.limitations() != input.limitations()
                    || value.reference_result().SerializeAsString()
                        != input.source().SerializeAsString()
                    || !value.retirement_reason().empty())
                    reject();
            } else if (request.mutation().has_logic()) {
                const auto& input = request.mutation().logic();
                if (!value.has_logic() || value.id() != input.id() || value.name() != input.name()
                    || value.purpose() != input.purpose()
                    || value.limitations() != input.limitations()
                    || value.logic().result().SerializeAsString()
                        != input.source().SerializeAsString()
                    || value.definition().strategy().SerializeAsString()
                        != input.strategy().SerializeAsString()
                    || value.definition().contract().SerializeAsString()
                        != input.contract().SerializeAsString()
                    || value.definition().interval_minutes() != input.interval_minutes()
                    || !value.retirement_reason().empty())
                    reject();
            } else {
                const auto& input = request.mutation().retire();
                if (strategy_package_object(value).SerializeAsString()
                        != input.package().SerializeAsString()
                    || value.retirement_reason() != input.reason())
                    reject();
            }
            result["package"] = decode_strategy_package(value);
        } else {
            const auto& value = reply.delivery();
            check_owner(value.owner());
            if (request.has_delivery()) {
                if (value.id() != request.delivery().id())
                    reject();
                result["target_accepted"] = reply.target_accepted();
            } else {
                const auto& input = request.mutation().deliver();
                if (value.id() != input.id() || value.target_task_id() != input.target_task_id()
                    || value.package().SerializeAsString() != input.package().SerializeAsString())
                    reject();
            }
            result["delivery"] = decode_strategy_delivery(value);
        }
    } else if (request.has_list() || request.has_deliveries()) {
        const auto& query = request.has_list() ? request.list() : request.deliveries();
        const auto size = request.has_list() ? reply.packages_size() : reply.deliveries_size();
        if (!query.limit() || query.limit() > 200 || size > static_cast<int>(query.limit())
            || (request.has_list() ? reply.deliveries_size() || !query.package_id().empty()
                                   : reply.packages_size()))
            reject();
        auto previous = query.after();
        for (const auto& value : reply.packages()) {
            check_owner(value.owner());
            if (value.id() <= previous)
                reject();
            result["packages"].push_back(decode_strategy_package(value));
            previous = value.id();
        }
        for (const auto& value : reply.deliveries()) {
            check_owner(value.owner());
            if (value.id() <= previous
                || (!query.package_id().empty() && value.package().id() != query.package_id()))
                reject();
            result["deliveries"].push_back(decode_strategy_delivery(value));
            previous = value.id();
        }
        if (!reply.next_after().empty()
            && (size != static_cast<int>(query.limit()) || reply.next_after() != previous))
            reject();
    } else {
        reject();
    }
    return result;
}

} // namespace asterion::protocol
