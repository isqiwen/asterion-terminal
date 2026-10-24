#include <asterion/protocol/research_batch.hpp>
#include <asterion/protocol/backtest.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/protocol/task.hpp>
#include <limits>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace asterion::protocol {
namespace {

bool text(const std::string& value, std::size_t limit)
{
    return !value.empty() && value.size() <= limit && value.find(char(0)) == std::string::npos
        && !std::ranges::all_of(value, [](unsigned char c) {
               return std::isspace(c);
           });
}

bool hash(const std::string& value)
{
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

} // namespace

void validate_backtest_batch_draft(const research::proto::BacktestBatchDraft& value)
{
    validate_message(value);
    validate_id(value.id());
    validate_strategy_object(value.source(), "backtest_result");
    if (!text(value.name(), 256) || !text(value.purpose(), 4096) || !text(value.limitations(), 4096)
        || !value.members_size() || value.members_size() > 100 || !value.run_budget()
        || value.run_budget() > 100
        || static_cast<unsigned>(value.members_size()) > value.run_budget()
        || !value.time_budget_seconds() || value.ByteSizeLong() > 65536)
        throw std::invalid_argument("invalid finite backtest batch or budget");
    std::set<std::string> tasks;
    std::set<std::pair<unsigned, std::int64_t>> conditions;
    for (const auto& member : value.members()) {
        validate_id(member.task_id());
        const auto fee = member.has_fee_multiplier()
            ? Decimal::from_raw(member.fee_multiplier().units())
            : Decimal::parse("1");
        if (member.task_id() == value.source().id() || !tasks.insert(member.task_id()).second
            || member.slippage_ticks() > 100 || fee < Decimal{} || fee > Decimal::parse("100")
            || !conditions.insert({member.slippage_ticks(), fee.raw()}).second)
            throw std::invalid_argument("backtest batch contains duplicate or invalid conditions");
    }
}

void validate_backtest_batch_definition(const research::proto::BacktestBatchDefinition& value)
{
    validate_message(value);
    validate_backtest_batch_draft(value.request());
    validate_id(value.data_owner().node_id());
    validate_id(value.data_owner().service_id());
    if (value.owner().SerializeAsString() != value.request().source().owner().SerializeAsString()
        || !hash(value.source_input_sha256()) || !hash(value.program_artifact())
        || !hash(value.risk_artifact()) || !value.work_units_per_run() || value.recorded_at_ms() < 0
        || value.digest() != backtest_batch_digest(value))
        throw std::invalid_argument("invalid fixed backtest batch definition");
}

void validate_backtest_batch_mutation(const research::proto::BacktestBatchMutation& value)
{
    validate_message(value);
    validate_id(value.id());
    if (value.has_save())
        validate_backtest_batch_draft(value.save());
    else if (value.has_start())
        validate_strategy_object(value.start(), "backtest_batch");
    else if (value.has_stop()) {
        validate_strategy_object(value.stop().batch(), "backtest_batch");
        if (!text(value.stop().reason(), 4096))
            throw std::invalid_argument("backtest batch stop reason required");
    } else
        throw std::invalid_argument("backtest batch operation required");
}

void validate_backtest_batch_state(const research::proto::BacktestBatchState& value)
{
    validate_message(value);
    validate_backtest_batch_definition(value.definition());
    if (value.phase() < research::proto::BATCH_SAVED
        || value.phase() > research::proto::BATCH_INTERRUPTED || value.started_at_ms() < 0
        || value.stopped_at_ms() < 0 || value.reason().size() > 4096
        || value.members_size() != value.definition().request().members_size()
        || value.ByteSizeLong() > 262144)
        throw std::invalid_argument("invalid backtest batch state");
    bool waiting = true, complete = true;
    for (int i = 0; i < value.members_size(); ++i) {
        const auto& member = value.members(i);
        if (member.task_id() != value.definition().request().members(i).task_id()
            || member.admission() < research::proto::BATCH_MEMBER_WAITING
            || member.admission() > research::proto::BATCH_MEMBER_REJECTED
            || (member.admission() == research::proto::BATCH_MEMBER_WAITING
                    ? !member.input_sha256().empty()
                    : member.admission() == research::proto::BATCH_MEMBER_REJECTED
                    ? !member.input_sha256().empty() && !hash(member.input_sha256())
                    : !hash(member.input_sha256()))
            || (member.admission() == research::proto::BATCH_MEMBER_REJECTED ? !member.has_error()
                        || !text(member.error().code(), 64) || !text(member.error().message(), 2048)
                                                                             : member.has_error()))
            throw std::invalid_argument("invalid backtest batch member receipt");
        waiting = waiting && member.admission() == research::proto::BATCH_MEMBER_WAITING;
        complete = complete
            && (member.admission() == research::proto::BATCH_MEMBER_ADMITTED
                || member.admission() == research::proto::BATCH_MEMBER_REJECTED);
    }
    if ((value.phase() == research::proto::BATCH_SAVED
            && (!waiting || value.started_at_ms() || value.stopped_at_ms() || value.has_elapsed_ms()
                || !value.reason().empty()))
        || (value.phase() == research::proto::BATCH_RUNNING
            && (value.stopped_at_ms() || value.has_elapsed_ms() || !value.reason().empty()))
        || (value.phase() == research::proto::BATCH_COMPLETED && !complete)
        || ((value.phase() == research::proto::BATCH_STOPPING
                || value.phase() == research::proto::BATCH_STOPPED
                || value.phase() == research::proto::BATCH_INTERRUPTED)
            && !text(value.reason(), 4096)))
        throw std::invalid_argument("inconsistent backtest batch control state");
}

std::string backtest_batch_digest(research::proto::BacktestBatchDefinition value)
{
    value.clear_digest();
    value.clear_recorded_at_ms();
    return sha256_bytes(value.SerializeAsString());
}

resource::proto::Object backtest_batch_object(const research::proto::BacktestBatchDefinition& value)
{
    validate_backtest_batch_definition(value);
    resource::proto::Object result;
    *result.mutable_owner() = value.owner();
    result.set_id(value.request().id());
    result.set_kind("backtest_batch");
    result.set_version(value.digest());
    return result;
}

backtest::proto::BacktestReuse backtest_batch_member_reuse(
    const research::proto::BacktestBatchDefinition& definition,
    std::string_view task_id)
{
    validate_backtest_batch_definition(definition);
    const auto member = std::ranges::find(
        definition.request().members(),
        task_id,
        &research::proto::BacktestBatchMember::task_id);
    if (member == definition.request().members().end())
        throw std::invalid_argument("task is not a member of this fixed backtest batch");
    backtest::proto::BacktestReuse result;
    result.set_source_id(definition.request().source().id());
    result.set_source_result_digest(definition.request().source().version());
    result.set_source_input_sha256(definition.source_input_sha256());
    result.set_choice(backtest::proto::BacktestReuse::FIXED);
    if (member->has_fee_multiplier())
        *result.mutable_fee_multiplier() = member->fee_multiplier();
    else
        result.set_slippage_ticks(member->slippage_ticks());
    return result;
}

backtest::proto::BacktestInput backtest_batch_member_input(
    const backtest::proto::BacktestInput& original,
    const research::proto::BacktestBatchMember& member)
{
    if (member.has_fee_multiplier()) {
        if (original.paper().contracts_size() != 1
            || member.slippage_ticks() != original.paper().contracts(0).slippage_ticks())
            throw std::invalid_argument("fee scenario must preserve original slippage");
        return backtest_fee_variant(original, Decimal::from_raw(member.fee_multiplier().units()));
    }
    return backtest_slippage_variant(original, member.slippage_ticks());
}

research::proto::BacktestBatchDraft encode_backtest_batch_draft(const Json& value)
{
    require_fields(
        value,
        {"id",
            "name",
            "purpose",
            "limitations",
            "source",
            "members",
            "run_budget",
            "time_budget_seconds"});
    const auto number = [](const Json& field) -> unsigned {
        if (!field.is_number_integer()
            || (field.is_number_integer() && !field.is_number_unsigned()
                && field.get<std::int64_t>() < 0)
            || field.get<std::uint64_t>() > std::numeric_limits<unsigned>::max())
            throw std::invalid_argument(
                "backtest batch requires unsigned integer budgets and ticks");
        return field.get<unsigned>();
    };
    research::proto::BacktestBatchDraft result;
    result.set_id(value.at("id").get<std::string>());
    result.set_name(value.at("name").get<std::string>());
    result.set_purpose(value.at("purpose").get<std::string>());
    result.set_limitations(value.at("limitations").get<std::string>());
    *result.mutable_source() = encode_strategy_object(value.at("source"), "backtest_result");
    result.set_run_budget(number(value.at("run_budget")));
    result.set_time_budget_seconds(number(value.at("time_budget_seconds")));
    if (!value.at("members").is_array())
        throw std::invalid_argument("backtest batch members must be an array");
    for (const auto& row : value.at("members")) {
        auto required = row;
        required.erase("fee_multiplier");
        require_fields(required, {"task_id", "slippage_ticks"});
        auto& member = *result.add_members();
        member.set_task_id(row.at("task_id").get<std::string>());
        member.set_slippage_ticks(number(row.at("slippage_ticks")));
        if (row.contains("fee_multiplier"))
            member.mutable_fee_multiplier()->set_units(
                Decimal::parse(row.at("fee_multiplier").get<std::string>()).raw());
    }
    validate_backtest_batch_draft(result);
    return result;
}

Json decode_backtest_batch_state(const research::proto::BacktestBatchState& value)
{
    validate_backtest_batch_state(value);
    const auto& definition = value.definition();
    const auto& request = definition.request();
    Json planned = Json::array(), members = Json::array();
    for (const auto& row : request.members()) {
        Json item{{"task_id", row.task_id()}, {"slippage_ticks", row.slippage_ticks()}};
        if (row.has_fee_multiplier())
            item["fee_multiplier"] = Decimal::from_raw(row.fee_multiplier().units()).str();
        planned.push_back(std::move(item));
    }
    for (const auto& row : value.members()) {
        const auto admission = row.admission() == research::proto::BATCH_MEMBER_WAITING ? "waiting"
            : row.admission() == research::proto::BATCH_MEMBER_PENDING                  ? "pending"
            : row.admission() == research::proto::BATCH_MEMBER_ADMITTED                 ? "admitted"
                                                                        : "rejected";
        members.push_back(
            {{"task_id", row.task_id()},
                {"admission", admission},
                {"input_sha256",
                    row.input_sha256().empty() ? Json(nullptr) : Json(row.input_sha256())},
                {"error",
                    row.has_error()
                        ? Json{{"code", row.error().code()}, {"message", row.error().message()}}
                        : Json(nullptr)}});
    }
    const auto phase = value.phase() == research::proto::BATCH_SAVED ? "saved"
        : value.phase() == research::proto::BATCH_RUNNING            ? "running"
        : value.phase() == research::proto::BATCH_STOPPING           ? "stopping"
        : value.phase() == research::proto::BATCH_STOPPED            ? "stopped"
        : value.phase() == research::proto::BATCH_COMPLETED          ? "completed"
                                                                     : "interrupted";
    return {
        {"definition",
            {{"object", decode_strategy_object(backtest_batch_object(definition))},
                {"owner",
                    {{"domain", definition.owner().domain()},
                        {"node_id", definition.owner().node_id()},
                        {"service_id", definition.owner().service_id()}}},
                {"name", request.name()},
                {"purpose", request.purpose()},
                {"limitations", request.limitations()},
                {"source", decode_strategy_object(request.source())},
                {"members", std::move(planned)},
                {"run_budget", request.run_budget()},
                {"time_budget_seconds", request.time_budget_seconds()},
                {"source_input_sha256", definition.source_input_sha256()},
                {"program_artifact", definition.program_artifact()},
                {"risk_artifact", definition.risk_artifact()},
                {"data_owner",
                    {{"node_id", definition.data_owner().node_id()},
                        {"service_id", definition.data_owner().service_id()}}},
                {"work_units_per_run", std::to_string(definition.work_units_per_run())},
                {"recorded_at_ms", definition.recorded_at_ms()}}},
        {"phase", phase},
        {"started_at_ms", value.started_at_ms() ? Json(value.started_at_ms()) : Json(nullptr)},
        {"stopped_at_ms", value.stopped_at_ms() ? Json(value.stopped_at_ms()) : Json(nullptr)},
        {"elapsed_ms",
            value.has_elapsed_ms() ? Json(std::to_string(value.elapsed_ms())) : Json(nullptr)},
        {"reason", value.reason()},
        {"members", std::move(members)}
    };
}

Json decode_backtest_batch_reply(
    const task::proto::ResearchBatchReply& reply,
    const research::proto::BacktestBatchRequest& request,
    const resource::proto::Owner& expected)
{
    validate_message(reply);
    validate_message(request);
    const auto reject = [] {
        throw std::invalid_argument("invalid research batch response");
    };
    const auto owned = [&](const research::proto::BacktestBatchState& state) {
        validate_backtest_batch_state(state);
        if (state.definition().owner().SerializeAsString() != expected.SerializeAsString())
            reject();
    };
    Json result =
        {{"states", Json::array()}, {"members", Json::array()}, {"next_after", reply.next_after()}};
    const bool single = request.has_read() || request.has_mutation();
    if ((!single && !request.has_list()) || reply.has_state() != single)
        reject();
    if (single) {
        if (reply.states_size() || !reply.next_after().empty())
            reject();
        const auto& state = reply.state();
        owned(state);
        if (request.has_read()) {
            if (request.read().id() != state.definition().request().id())
                reject();
        } else {
            const auto& command = request.mutation();
            validate_backtest_batch_mutation(command);
            if (command.has_save()) {
                if (state.definition().request().SerializeAsString()
                    != command.save().SerializeAsString())
                    reject();
            } else if (backtest_batch_object(state.definition()).SerializeAsString()
                != (command.has_start() ? command.start() : command.stop().batch())
                    .SerializeAsString())
                reject();
        }
        result["state"] = decode_backtest_batch_state(state);
        std::set<std::string> seen;
        const auto object = backtest_batch_object(state.definition());
        for (const auto& task : reply.members()) {
            const auto member = std::ranges::find(
                state.members(),
                task.id(),
                &research::proto::BacktestBatchMemberReceipt::task_id);
            const auto& origin = task.metadata().derivation();
            if (!seen.insert(task.id()).second || member == state.members().end()
                || (member->admission() != research::proto::BATCH_MEMBER_ADMITTED
                    && member->admission() != research::proto::BATCH_MEMBER_PENDING)
                || task.kind() != task::proto::BACKTEST
                || origin.batch().SerializeAsString() != object.SerializeAsString()
                || origin.source_id() != state.definition().request().source().id()
                || origin.source_result_digest() != state.definition().request().source().version()
                || origin.source_instance() != expected.service_id()
                || origin.data_choice() != "fixed" || !origin.snapshot().empty()
                || task.program_artifact() != state.definition().program_artifact()
                || task.risk_artifact() != state.definition().risk_artifact())
                reject();
            const bool terminal = task.state() == task::proto::SUCCEEDED
                || task.state() == task::proto::FAILED || task.state() == task::proto::CANCELLED
                || task.state() == task::proto::INTERRUPTED;
            if ((state.phase() == research::proto::BATCH_COMPLETED
                    || state.phase() == research::proto::BATCH_STOPPED)
                && !terminal)
                reject();
            result["members"].push_back(decode_task(task));
        }
        for (const auto& member : state.members())
            if (member.admission() == research::proto::BATCH_MEMBER_ADMITTED
                && !seen.contains(member.task_id()))
                reject();
    } else {
        const auto& page = request.list();
        if (!page.after().empty())
            validate_id(page.after());
        if (!page.limit() || page.limit() > 200
            || reply.states_size() > static_cast<int>(page.limit()) || reply.members_size())
            reject();
        auto previous = page.after();
        for (const auto& state : reply.states()) {
            owned(state);
            const auto id = state.definition().request().id();
            if (id <= previous)
                reject();
            result["states"].push_back(decode_backtest_batch_state(state));
            previous = id;
        }
        if (!reply.next_after().empty()
            && (reply.states_size() != static_cast<int>(page.limit())
                || reply.next_after() != previous))
            reject();
    }
    return result;
}

} // namespace asterion::protocol
