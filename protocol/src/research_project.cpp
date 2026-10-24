#include <asterion/protocol/research_project.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/foundation/id.hpp>

namespace asterion::protocol {

void validate_project_binding(const research::proto::ProjectBinding& value)
{
    validate_message(value);
    validate_id(value.project_id());
    validate_id(value.plan_id());
    validate_id(value.owner().node_id());
    validate_id(value.owner().service_id());
    if (value.owner().domain() != "research" || value.definition_digest().size() != 64
        || value.definition_digest().find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid fixed research plan identity");
}

Json decode_project_binding(const research::proto::ProjectBinding& value)
{
    validate_project_binding(value);
    return {
        {"project_id", value.project_id()},
        {"plan_id", value.plan_id()},
        {"definition_digest", value.definition_digest()},
        {"owner",
            {{"domain", value.owner().domain()},
                {"node_id", value.owner().node_id()},
                {"service_id", value.owner().service_id()}}}
    };
}

research::proto::ProjectBinding encode_project_binding(const Json& value)
{
    require_fields(value, {"project_id", "plan_id", "definition_digest", "owner"});
    const auto& owner = value.at("owner");
    require_fields(owner, {"domain", "node_id", "service_id"});
    research::proto::ProjectBinding result;
    result.set_project_id(value.at("project_id").get<std::string>());
    result.set_plan_id(value.at("plan_id").get<std::string>());
    result.set_definition_digest(value.at("definition_digest").get<std::string>());
    result.mutable_owner()->set_domain(owner.at("domain").get<std::string>());
    result.mutable_owner()->set_node_id(owner.at("node_id").get<std::string>());
    result.mutable_owner()->set_service_id(owner.at("service_id").get<std::string>());
    validate_project_binding(result);
    return result;
}

research::proto::ProjectDefinition encode_project_definition(const Json& value)
{
    require_fields(
        value,
        {"title",
            "question",
            "hypothesis",
            "mechanism",
            "scope",
            "data_needs",
            "evaluation",
            "support",
            "refutation",
            "limitations",
            "notes",
            "sources"});
    research::proto::ProjectDefinition result;
    result.set_title(value.at("title").get<std::string>());
    result.set_question(value.at("question").get<std::string>());
    result.set_hypothesis(value.at("hypothesis").get<std::string>());
    result.set_mechanism(value.at("mechanism").get<std::string>());
    result.set_scope(value.at("scope").get<std::string>());
    result.set_data_needs(value.at("data_needs").get<std::string>());
    result.set_evaluation(value.at("evaluation").get<std::string>());
    result.set_support(value.at("support").get<std::string>());
    result.set_refutation(value.at("refutation").get<std::string>());
    result.set_limitations(value.at("limitations").get<std::string>());
    result.set_notes(value.at("notes").get<std::string>());
    if (!value.at("sources").is_array())
        throw std::invalid_argument("invalid research project definition");
    for (const auto& source : value.at("sources"))
        result.add_sources(source.get<std::string>());
    return result;
}

Json decode_project_definition(const research::proto::ProjectDefinition& value)
{
    validate_message(value);
    return {
        {"title", value.title()},
        {"question", value.question()},
        {"hypothesis", value.hypothesis()},
        {"mechanism", value.mechanism()},
        {"scope", value.scope()},
        {"data_needs", value.data_needs()},
        {"evaluation", value.evaluation()},
        {"support", value.support()},
        {"refutation", value.refutation()},
        {"limitations", value.limitations()},
        {"notes", value.notes()},
        {"sources", std::vector<std::string>(value.sources().begin(), value.sources().end())}
    };
}

research::proto::ConclusionWrite encode_project_conclusion(const Json& value)
{
    require_fields(
        value,
        {"id", "plan", "result", "judgment", "reason", "previous_id", "revision_reason"});
    research::proto::ConclusionWrite result;
    result.set_id(value.at("id").get<std::string>());
    *result.mutable_plan() = encode_project_binding(value.at("plan"));
    const auto& ref = value.at("result");
    require_fields(ref, {"node", "service", "domain", "kind", "id", "version"});
    auto& object = *result.mutable_result();
    object.mutable_owner()->set_node_id(ref.at("node").get<std::string>());
    object.mutable_owner()->set_service_id(ref.at("service").get<std::string>());
    object.mutable_owner()->set_domain(ref.at("domain").get<std::string>());
    object.set_kind(ref.at("kind").get<std::string>());
    object.set_id(ref.at("id").get<std::string>());
    object.set_version(ref.at("version").get<std::string>());
    const auto judgment = value.at("judgment").get<std::string>();
    if (judgment == "supported")
        result.set_judgment(research::proto::SUPPORTED);
    else if (judgment == "refuted")
        result.set_judgment(research::proto::REFUTED);
    else if (judgment == "insufficient")
        result.set_judgment(research::proto::INSUFFICIENT);
    else
        throw std::invalid_argument("invalid research conclusion");
    result.set_reason(value.at("reason").get<std::string>());
    result.set_previous_id(value.at("previous_id").get<std::string>());
    result.set_revision_reason(value.at("revision_reason").get<std::string>());
    return result;
}

Json decode_project_reply(
    const research::proto::ProjectReply& reply,
    const research::proto::ProjectRequest& request,
    const resource::proto::Owner& expected)
{
    validate_message(reply);
    validate_message(request);
    const auto reject = [] {
        throw Error(ErrorCode::unavailable, "invalid research project response");
    };
    const auto hash = [](const std::string& value) {
        return value.size() == 64
            && value.find_first_not_of("0123456789abcdef") == std::string::npos;
    };
    const auto own = [&](const resource::proto::Owner& owner) {
        if (owner.SerializeAsString() != expected.SerializeAsString())
            throw Error(ErrorCode::conflict, "research project owner mismatch");
        return Json{
            {"domain", owner.domain()},
            {"node_id", owner.node_id()},
            {"service_id", owner.service_id()}
        };
    };
    const auto draft = [&](const research::proto::ProjectDraft& value) {
        validate_id(value.id());
        if (!hash(value.digest()) || value.updated_at_ms() < 0)
            reject();
        return Json{
            {"id", value.id()},
            {"owner", own(value.owner())},
            {"definition", decode_project_definition(value.definition())},
            {"digest", value.digest()},
            {"updated_at_ms", std::to_string(value.updated_at_ms())}
        };
    };
    const auto plan = [&](const research::proto::HypothesisPlan& value) {
        validate_id(value.id());
        validate_id(value.project_id());
        if (!hash(value.definition_digest()) || value.recorded_at_ms() < 0)
            reject();
        return Json{
            {"id", value.id()},
            {"project_id", value.project_id()},
            {"owner", own(value.owner())},
            {"definition", decode_project_definition(value.definition())},
            {"definition_digest", value.definition_digest()},
            {"recorded_at_ms", std::to_string(value.recorded_at_ms())},
            {"parent_plan_id", value.parent_plan_id()},
            {"revision_reason", value.revision_reason()},
            {"exploratory", value.exploratory()}
        };
    };
    const auto conclusion = [&](const research::proto::ProjectConclusion& value) {
        const auto& d = value.declaration();
        validate_id(d.id());
        validate_id(d.result().id());
        validate_project_binding(d.plan());
        (void)own(d.plan().owner());
        (void)own(d.result().owner());
        if (!hash(value.digest()) || !hash(value.input_sha256()) || !hash(value.program_artifact())
            || !hash(d.result().version())
            || (d.result().kind() != "factor_result" && d.result().kind() != "sample_result"
                && d.result().kind() != "statistics_result"
                && d.result().kind() != "backtest_result"
                && d.result().kind() != "performance_evaluation_result")
            || value.recorded_at_ms() < 0 || d.judgment() < research::proto::SUPPORTED
            || d.judgment() > research::proto::INSUFFICIENT || d.reason().empty())
            reject();
        return Json{
            {"declaration",
                {{"id", d.id()},
                    {"plan", decode_project_binding(d.plan())},
                    {"result",
                        {{"node", d.result().owner().node_id()},
                            {"service", d.result().owner().service_id()},
                            {"domain", d.result().owner().domain()},
                            {"kind", d.result().kind()},
                            {"id", d.result().id()},
                            {"version", d.result().version()}}},
                    {"judgment",
                        d.judgment() == research::proto::SUPPORTED     ? "supported"
                            : d.judgment() == research::proto::REFUTED ? "refuted"
                                                                       : "insufficient"},
                    {"reason", d.reason()},
                    {"previous_id", d.previous_id()},
                    {"revision_reason", d.revision_reason()}}},
            {"input_sha256", value.input_sha256()},
            {"program_artifact", value.program_artifact()},
            {"retrospective", value.retrospective()},
            {"recorded_at_ms", std::to_string(value.recorded_at_ms())},
            {"digest", value.digest()}
        };
    };
    Json result{
        {"conclusions", Json::array()},
        {"drafts", Json::array()},
        {"plans", Json::array()},
        {"next_after", reply.next_after()}
    };
    const bool wanted_draft =
        request.has_draft() || (request.has_mutation() && request.mutation().has_save());
    const bool wanted_plan =
        request.has_plan() || (request.has_mutation() && request.mutation().has_fix());
    const bool wanted_conclusion =
        request.has_conclusion() || (request.has_mutation() && request.mutation().has_conclude());
    if (reply.has_draft() != wanted_draft || reply.has_plan() != wanted_plan
        || reply.has_conclusion() != wanted_conclusion)
        reject();
    if (wanted_draft) {
        const auto id =
            request.has_draft() ? request.draft().id() : request.mutation().save().project_id();
        if (reply.draft().id() != id)
            reject();
        result["draft"] = draft(reply.draft());
    }
    if (wanted_plan) {
        const auto id =
            request.has_plan() ? request.plan().id() : request.mutation().fix().plan_id();
        if (reply.plan().id() != id)
            reject();
        result["plan"] = plan(reply.plan());
    }
    if (wanted_conclusion) {
        const auto id = request.has_conclusion() ? request.conclusion().id()
                                                 : request.mutation().conclude().id();
        if (reply.conclusion().declaration().id() != id
            || (request.has_mutation()
                && reply.conclusion().declaration().SerializeAsString()
                    != request.mutation().conclude().SerializeAsString()))
            reject();
        result["conclusion"] = conclusion(reply.conclusion());
    }
    if (request.has_conclusions()) {
        const auto& query = request.conclusions();
        if (!query.limit() || query.limit() > 200
            || reply.conclusions_size() > static_cast<int>(query.limit()) || reply.drafts_size()
            || reply.plans_size())
            reject();
        auto previous = query.after();
        for (const auto& row : reply.conclusions()) {
            if (row.declaration().id() <= previous
                || (!query.project_id().empty()
                    && row.declaration().plan().project_id() != query.project_id()))
                reject();
            result["conclusions"].push_back(conclusion(row));
            previous = row.declaration().id();
        }
        if (!reply.next_after().empty()
            && (reply.conclusions().empty() || reply.next_after() != previous))
            reject();
    } else if (reply.conclusions_size())
        reject();
    if (request.has_drafts()) {
        const auto& query = request.drafts();
        if (!query.limit() || query.limit() > 200
            || reply.drafts_size() > static_cast<int>(query.limit()) || reply.plans_size())
            reject();
        auto previous = query.after();
        for (const auto& row : reply.drafts()) {
            if (row.id() <= previous)
                reject();
            result["drafts"].push_back(draft(row));
            previous = row.id();
        }
        if (!reply.next_after().empty()
            && (reply.drafts().empty() || reply.next_after() != previous))
            reject();
    } else if (request.has_plans()) {
        const auto& query = request.plans();
        if (!query.limit() || query.limit() > 200
            || reply.plans_size() > static_cast<int>(query.limit()) || reply.drafts_size())
            reject();
        auto previous = query.after();
        for (const auto& row : reply.plans()) {
            if (row.id() <= previous
                || (!query.project_id().empty() && row.project_id() != query.project_id()))
                reject();
            result["plans"].push_back(plan(row));
            previous = row.id();
        }
        if (!reply.next_after().empty()
            && (reply.plans().empty() || reply.next_after() != previous))
            reject();
    } else if (reply.drafts_size() || reply.plans_size()
        || (!request.has_conclusions() && !reply.next_after().empty())
        || (!wanted_draft && !wanted_plan && !wanted_conclusion && !request.has_conclusions()))
        reject();
    return result;
}

} // namespace asterion::protocol
