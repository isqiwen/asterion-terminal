#include <asterion/protocol/research_assessment.hpp>
#include <asterion/protocol/research_batch.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <cctype>
#include <asterion/protocol/trading.hpp>
#include <asterion/domain/trading_days.hpp>
#include <asterion/foundation/decimal.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace asterion::protocol {
namespace {
namespace r = research::proto;
namespace t = task::proto;

bool hash(const std::string& value)
{
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

void text(const std::string& value, std::size_t limit = 4096)
{
    if (value.empty() || value.size() > limit)
        throw std::invalid_argument("invalid batch assessment text");
}

void observation(const r::BatchAssessmentTask& value)
{
    validate_id(value.id());
    if (value.state() < t::QUEUED || value.state() > t::PUBLISHING || !hash(value.input_sha256())
        || !hash(value.program_artifact()) || !hash(value.risk_artifact())
        || (value.state() == t::SUCCEEDED
                ? (!value.attempt() || !hash(value.result_sha256()) || !value.has_account())
                : (!value.result_sha256().empty() || value.has_account())))
        throw std::invalid_argument("invalid batch assessment task observation");
    if (!value.has_account())
        return;
    const auto& account = value.account();
    (void)parse_trading_date(account.first_day());
    (void)parse_trading_date(account.last_day());
    if (account.currency().empty() || account.currency().size() > 64 || !account.settlement_days()
        || account.first_day() > account.last_day() || !account.has_deposit()
        || !account.has_closing_equity() || !account.has_fees() || !account.has_net_profit()
        || account.net_profit().units()
            != (Decimal::from_raw(account.closing_equity().units())
                - Decimal::from_raw(account.deposit().units()))
                .raw())
        throw std::invalid_argument("invalid batch assessment account extract");
}

const r::BatchAssessmentTask* selected(const r::BatchAssessment& value)
{
    const auto& id = value.declaration().selected_task_id();
    if (id == value.snapshot().source().id())
        return &value.snapshot().source();
    const auto found = std::ranges::find_if(value.snapshot().members(), [&](const auto& member) {
        return member.id() == id;
    });
    return found == value.snapshot().members().end() ? nullptr : &*found;
}

} // namespace

void validate_batch_assessment_snapshot(const r::BatchAssessmentSnapshot& value)
{
    validate_message(value);
    validate_backtest_batch_state(value.batch());
    observation(value.source());
    const auto& definition = value.batch().definition();
    if (value.ByteSizeLong() > 4194304 || value.source().id() != definition.request().source().id()
        || value.source().state() != t::SUCCEEDED
        || value.source().result_sha256() != definition.request().source().version()
        || value.source().input_sha256() != definition.source_input_sha256()
        || value.source().program_artifact() != definition.program_artifact()
        || value.source().risk_artifact() != definition.risk_artifact())
        throw std::invalid_argument("batch assessment source differs from fixed manifest");
    std::set<std::string> seen;
    for (const auto& task : value.members()) {
        observation(task);
        const auto found = std::ranges::find_if(value.batch().members(), [&](const auto& member) {
            return member.task_id() == task.id();
        });
        if (!seen.insert(task.id()).second || found == value.batch().members().end()
            || (found->admission() != r::BATCH_MEMBER_ADMITTED
                && found->admission() != r::BATCH_MEMBER_PENDING)
            || found->input_sha256() != task.input_sha256()
            || task.program_artifact() != definition.program_artifact()
            || task.risk_artifact() != definition.risk_artifact())
            throw std::invalid_argument("batch assessment member differs from fixed manifest");
    }
    for (const auto& member : value.batch().members())
        if (member.admission() == r::BATCH_MEMBER_ADMITTED && !seen.contains(member.task_id()))
            throw std::invalid_argument("batch assessment is missing an admitted task");
}

void validate_batch_assessment_declaration(const r::BatchAssessmentDeclaration& value)
{
    validate_message(value);
    validate_id(value.id());
    validate_id(value.batch().owner().node_id());
    validate_id(value.batch().owner().service_id());
    validate_id(value.batch().id());
    if (value.batch().kind() != "backtest_batch" || value.batch().owner().domain() != "research"
        || !hash(value.batch().version()) || !hash(value.expected_snapshot())
        || value.judgment() < r::SUPPORTED || value.judgment() > r::INSUFFICIENT
        || value.ByteSizeLong() > 65536)
        throw std::invalid_argument("invalid fixed batch assessment declaration");
    text(value.question());
    text(value.scope());
    text(value.reason());
    text(value.selection_reason());
    text(value.unselected_reason());
    text(value.information_use());
    if (!value.selected_task_id().empty())
        validate_id(value.selected_task_id());
    if (value.previous_id().empty() != value.revision_reason().empty()
        || value.previous_id() == value.id())
        throw std::invalid_argument("invalid batch assessment revision");
    if (!value.previous_id().empty()) {
        validate_id(value.previous_id());
        text(value.revision_reason());
    }
}

std::string batch_assessment_snapshot_digest(const r::BatchAssessmentSnapshot& value)
{
    validate_batch_assessment_snapshot(value);
    return sha256_bytes(value.SerializeAsString());
}

std::string batch_assessment_digest(r::BatchAssessment value)
{
    value.clear_digest();
    return sha256_bytes(value.SerializeAsString());
}

void validate_batch_assessment(const r::BatchAssessment& value)
{
    validate_message(value);
    validate_batch_assessment_declaration(value.declaration());
    validate_batch_assessment_snapshot(value.snapshot());
    if (value.owner().SerializeAsString()
            != value.snapshot().batch().definition().owner().SerializeAsString()
        || value.declaration().batch().SerializeAsString()
            != backtest_batch_object(value.snapshot().batch().definition()).SerializeAsString()
        || value.declaration().expected_snapshot()
            != batch_assessment_snapshot_digest(value.snapshot())
        || !hash(value.program_artifact()) || value.recorded_at_ms() <= 0
        || value.digest() != batch_assessment_digest(value))
        throw std::invalid_argument("batch assessment identity differs from fixed evidence");
    if (!value.declaration().selected_task_id().empty()) {
        const auto* choice = selected(value);
        if (!choice || choice->state() != t::SUCCEEDED)
            throw std::invalid_argument(
                "batch assessment selection requires a verified successful result");
    }
}

resource::proto::Object batch_assessment_object(const r::BatchAssessment& value)
{
    validate_batch_assessment(value);
    resource::proto::Object result;
    *result.mutable_owner() = value.owner();
    result.set_kind("backtest_batch_assessment");
    result.set_id(value.declaration().id());
    result.set_version(value.digest());
    return result;
}

namespace {

Json assessment_task_json(const r::BatchAssessmentTask& value)
{
    auto state = t::TaskState_Name(value.state());
    std::ranges::transform(state, state.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    Json result = {
        {"id", value.id()},
        {"state", state},
        {"attempt", value.attempt()},
        {"retired", value.retired()},
        {"error", value.error()},
        {"input_sha256", value.input_sha256()},
        {"result_sha256",
            value.result_sha256().empty() ? Json(nullptr) : Json(value.result_sha256())},
        {"program_artifact", value.program_artifact()},
        {"risk_artifact", value.risk_artifact()},
        {"account", nullptr}
    };
    if (value.has_account()) {
        const auto& account = value.account();
        result["account"] = {
            {"currency", account.currency()},
            {"first_day", account.first_day()},
            {"last_day", account.last_day()},
            {"settlement_days", account.settlement_days()},
            {"deposit", Decimal::from_raw(account.deposit().units()).str()},
            {"closing_equity", Decimal::from_raw(account.closing_equity().units()).str()},
            {"fees", Decimal::from_raw(account.fees().units()).str()},
            {"net_profit", Decimal::from_raw(account.net_profit().units()).str()}
        };
    }
    return result;
}

Json assessment_snapshot_json(const r::BatchAssessmentSnapshot& value)
{
    validate_batch_assessment_snapshot(value);
    Json members = Json::array();
    for (const auto& task : value.members())
        members.push_back(assessment_task_json(task));
    return {
        {"batch", decode_backtest_batch_state(value.batch())},
        {"source", assessment_task_json(value.source())},
        {"members", std::move(members)}
    };
}

Json assessment_declaration_json(const r::BatchAssessmentDeclaration& value)
{
    validate_batch_assessment_declaration(value);
    const auto judgment = value.judgment() == r::SUPPORTED ? "supported"
        : value.judgment() == r::REFUTED                   ? "refuted"
                                                           : "insufficient";
    return {
        {"id", value.id()},
        {"batch", decode_strategy_object(value.batch())},
        {"expected_snapshot", value.expected_snapshot()},
        {"question", value.question()},
        {"scope", value.scope()},
        {"judgment", judgment},
        {"reason", value.reason()},
        {"selected_task_id", value.selected_task_id()},
        {"selection_reason", value.selection_reason()},
        {"unselected_reason", value.unselected_reason()},
        {"validation_information_used", value.validation_information_used()},
        {"information_use", value.information_use()},
        {"previous_id", value.previous_id()},
        {"revision_reason", value.revision_reason()}
    };
}

} // namespace

r::BatchAssessmentDeclaration encode_batch_assessment_declaration(const Json& value)
{
    require_fields(
        value,
        {"id",
            "batch",
            "expected_snapshot",
            "question",
            "scope",
            "judgment",
            "reason",
            "selected_task_id",
            "selection_reason",
            "unselected_reason",
            "validation_information_used",
            "information_use",
            "previous_id",
            "revision_reason"});
    r::BatchAssessmentDeclaration result;
    result.set_id(value.at("id").get<std::string>());
    *result.mutable_batch() = encode_strategy_object(value.at("batch"), "backtest_batch");
    result.set_expected_snapshot(value.at("expected_snapshot").get<std::string>());
    result.set_question(value.at("question").get<std::string>());
    result.set_scope(value.at("scope").get<std::string>());
    const auto judgment = value.at("judgment").get<std::string>();
    if (judgment == "supported")
        result.set_judgment(r::SUPPORTED);
    else if (judgment == "refuted")
        result.set_judgment(r::REFUTED);
    else if (judgment == "insufficient")
        result.set_judgment(r::INSUFFICIENT);
    else
        throw std::invalid_argument("invalid fixed batch assessment declaration");
    result.set_reason(value.at("reason").get<std::string>());
    result.set_selected_task_id(value.at("selected_task_id").get<std::string>());
    result.set_selection_reason(value.at("selection_reason").get<std::string>());
    result.set_unselected_reason(value.at("unselected_reason").get<std::string>());
    result.set_validation_information_used(value.at("validation_information_used").get<bool>());
    result.set_information_use(value.at("information_use").get<std::string>());
    result.set_previous_id(value.at("previous_id").get<std::string>());
    result.set_revision_reason(value.at("revision_reason").get<std::string>());
    validate_batch_assessment_declaration(result);
    return result;
}

Json decode_batch_assessment(const r::BatchAssessment& value)
{
    validate_batch_assessment(value);
    return {
        {"object", decode_strategy_object(batch_assessment_object(value))},
        {"declaration", assessment_declaration_json(value.declaration())},
        {"snapshot", assessment_snapshot_json(value.snapshot())},
        {"owner",
            {{"node_id", value.owner().node_id()},
                {"service_id", value.owner().service_id()},
                {"domain", value.owner().domain()}}},
        {"program_artifact", value.program_artifact()},
        {"recorded_at_ms", std::to_string(value.recorded_at_ms())}
    };
}

Json decode_batch_assessment_reply(
    const r::BatchAssessmentReply& reply,
    const r::BatchAssessmentRequest& request,
    const resource::proto::Owner& expected)
{
    validate_message(reply);
    validate_message(request);
    const auto reject = [] {
        throw std::invalid_argument("invalid batch assessment response");
    };
    const auto owned = [&](const r::BatchAssessment& value) {
        validate_batch_assessment(value);
        if (value.owner().SerializeAsString() != expected.SerializeAsString())
            reject();
    };
    Json result = {
        {"preview", nullptr},
        {"preview_digest", ""},
        {"assessment", nullptr},
        {"assessments", Json::array()},
        {"next_after", reply.next_after()}
    };
    if (request.has_preview()) {
        if (!reply.has_preview() || reply.has_assessment() || !reply.assessments().empty()
            || !reply.next_after().empty()
            || reply.preview_digest() != batch_assessment_snapshot_digest(reply.preview())
            || request.preview().SerializeAsString()
                != backtest_batch_object(reply.preview().batch().definition()).SerializeAsString()
            || reply.preview().batch().definition().owner().SerializeAsString()
                != expected.SerializeAsString())
            reject();
        result["preview"] = assessment_snapshot_json(reply.preview());
        result["preview_digest"] = reply.preview_digest();
    } else if (request.has_mutation() || request.has_read()) {
        if (!reply.has_assessment() || reply.has_preview() || !reply.preview_digest().empty()
            || !reply.assessments().empty() || !reply.next_after().empty())
            reject();
        owned(reply.assessment());
        if (request.has_read() ? reply.assessment().declaration().id() != request.read()
                               : reply.assessment().declaration().SerializeAsString()
                    != request.mutation().save().SerializeAsString())
            reject();
        result["assessment"] = decode_batch_assessment(reply.assessment());
    } else if (request.has_list()) {
        const auto& page = request.list();
        if (!page.limit() || page.limit() > 200 || reply.has_preview() || reply.has_assessment()
            || !reply.preview_digest().empty()
            || reply.assessments_size() > static_cast<int>(page.limit()))
            reject();
        auto after = page.after();
        for (const auto& value : reply.assessments()) {
            owned(value);
            if (value.declaration().id() <= after
                || (!page.batch_id().empty()
                    && value.declaration().batch().id() != page.batch_id()))
                reject();
            after = value.declaration().id();
            result["assessments"].push_back(decode_batch_assessment(value));
        }
        if (!reply.next_after().empty()
            && (reply.assessments().empty() || reply.next_after() != after))
            reject();
    } else
        reject();
    return result;
}

} // namespace asterion::protocol
