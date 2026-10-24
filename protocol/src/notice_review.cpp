#include <asterion/protocol/notice_review.hpp>
#include <asterion/protocol/intelligence.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <set>
#include <algorithm>

namespace asterion::protocol {
namespace {

bool same(const google::protobuf::Message& a, const google::protobuf::Message& b)
{
    return a.SerializeAsString() == b.SerializeAsString();
}

void digest(const std::string& v)
{
    if (v.size() != 64 || v.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("notice review requires a fixed execution program");
}

void owner(const resource::proto::Owner& v)
{
    validate_id(v.node_id());
    validate_id(v.service_id());
    if (v.domain() != "research")
        throw std::invalid_argument("notice review requires a Research owner");
}

Json object(const resource::proto::Object& v)
{
    return {
        {"node", v.owner().node_id()},
        {"service", v.owner().service_id()},
        {"domain", v.owner().domain()},
        {"kind", v.kind()},
        {"id", v.id()},
        {"version", v.version()}
    };
}

} // namespace

void validate_event_set_review_request(const research::proto::EventSetReviewRequest& v)
{
    validate_message(v);
    validate_id(v.source().owner().node_id());
    validate_id(v.source().owner().service_id());
    validate_id(v.source().id());
    digest(v.source().version());
    if (v.source().owner().domain() != "intelligence" || v.source().kind() != "intelligence_set"
        || v.use_scope().empty() || v.use_scope().size() > 4096
        || v.use_scope().find('\0') != std::string::npos)
        throw std::invalid_argument("invalid fixed event set review request");
}

void validate_event_set_review_execution(const research::proto::EventSetReviewExecution& v)
{
    validate_message(v);
    owner(v.owner());
    digest(v.program_artifact());
    validate_event_set_review_request(v.request());
    if (v.owner().node_id() != v.request().source().owner().node_id())
        throw std::invalid_argument("cross-node event set review is unavailable");
}

void validate_event_set_review_input(const research::proto::EventSetReviewInput& v)
{
    validate_message(v);
    research::proto::EventSetReviewExecution execution;
    *execution.mutable_owner() = v.owner();
    *execution.mutable_request() = v.request();
    execution.set_program_artifact(v.program_artifact());
    validate_event_set_review_execution(execution);
    validate_notice_set_record(v.publication());
    if (!v.publication().has_published())
        throw std::invalid_argument("event set review requires a published set");
    const auto& intent = v.publication().intent();
    const auto& payload = intent.payload();
    const auto& query = payload.request().query();
    const auto body = v.content().payload().SerializeAsString();
    if (!same(v.request().source(), v.publication().published().reference())
        || !same(v.content().receipt(), v.publication().published().storage())
        || !same(v.content().payload(), payload) || body.size() != v.content().receipt().bytes()
        || sha256_bytes(body) != v.content().receipt().content().version()
        || v.request().use_scope() != query.use_scope()
        || v.members_size() != payload.members_size())
        throw std::invalid_argument("event set review differs from fixed set content");
    int changes = 0;
    for (int i = 0; i < payload.members_size(); ++i) {
        const auto& member = payload.members(i);
        const auto& input = v.members(i);
        validate_notice_review_input(input);
        if (!same(input.owner(), v.owner()) || input.program_artifact() != v.program_artifact()
            || !same(input.publication(), member.publication())
            || input.request().as_of_ms() != query.as_of_ms()
            || input.request().use_scope() != query.use_scope())
            throw std::invalid_argument("event set review member differs from fixed selection");
        if (!member.event().has_latest_change())
            continue;
        if (changes >= v.changes_size())
            throw std::invalid_argument("event set review is missing fixed change evidence");
        const auto& change = member.event().latest_change();
        const auto& content = v.changes(changes++);
        validate_notice_change_record(change);
        const auto bytes = content.payload().SerializeAsString();
        if (!same(content.receipt(), change.published().storage())
            || !same(content.payload(), change.intent().payload())
            || bytes.size() != content.receipt().bytes()
            || sha256_bytes(bytes) != content.receipt().content().version())
            throw std::invalid_argument("event set review change differs from fixed evidence");
    }
    if (changes != v.changes_size())
        throw std::invalid_argument("event set review has unrelated change evidence");
}

void validate_event_set_review_result(
    const research::proto::EventSetReviewInput& input,
    const research::proto::EventSetReviewResult& v)
{
    validate_event_set_review_input(input);
    validate_message(v);
    const auto& payload = input.content().payload();
    const auto& query = payload.request().query();
    if (!same(v.owner(), input.owner()) || !same(v.source(), input.request().source())
        || !same(v.content(), input.content().receipt().content())
        || v.program_artifact() != input.program_artifact() || v.as_of_ms() != query.as_of_ms()
        || v.use_mode() != query.use_mode() || v.use_scope() != query.use_scope()
        || v.assembled_at_ms() != input.publication().published().available_at_ms()
        || v.members_size() != input.members_size()
        || v.exclusions_size() != payload.exclusions_size())
        throw std::invalid_argument("event set review result differs from fixed selection");
    for (int i = 0; i < v.members_size(); ++i)
        validate_notice_review_result(input.members(i), v.members(i));
    for (int i = 0; i < v.exclusions_size(); ++i)
        if (!same(v.exclusions(i), payload.exclusions(i)))
            throw std::invalid_argument("event set review result changes exclusion evidence");
}

Json decode_event_set_review_input(const research::proto::EventSetReviewInput& v)
{
    validate_event_set_review_input(v);
    const auto& payload = v.content().payload();
    Json events = Json::array(), exclusions = Json::array(), members = Json::array();
    for (const auto& event : payload.request().query().events())
        events.push_back(object(event));
    for (const auto& excluded : payload.exclusions()) {
        Json reasons = Json::array();
        for (const auto& reason : excluded.reasons())
            reasons.push_back({{"source", object(reason.source())}, {"reason", reason.reason()}});
        exclusions.push_back({{"event", object(excluded.event())}, {"reasons", reasons}});
    }
    for (int i = 0; i < v.members_size(); ++i) {
        Json limits = Json::array();
        for (const auto& limit : payload.members(i).limitations())
            limits.push_back({{"source", object(limit.source())}, {"reason", limit.reason()}});
        members.push_back(
            {{"event", decode_notice_event_view(payload.members(i).event())},
                {"evidence", decode_notice_review_input(v.members(i))},
                {"limitations", limits}});
    }
    return {
        {"owner",
            {{"node", v.owner().node_id()},
                {"service", v.owner().service_id()},
                {"domain", v.owner().domain()}}},
        {"source", object(v.request().source())},
        {"content", object(v.content().receipt().content())},
        {"program_artifact", v.program_artifact()},
        {"name", payload.request().name()},
        {"as_of_ms", std::to_string(payload.request().query().as_of_ms())},
        {"use_scope", v.request().use_scope()},
        {"use_mode",
            payload.request().query().use_mode() == intelligence::proto::STRICT_HISTORY
                ? "strict_history"
                : "post_hoc"},
        {"assembled_at_ms", std::to_string(v.publication().published().available_at_ms())},
        {"events", events},
        {"members", members},
        {"exclusions", exclusions}
    };
}

Json decode_event_set_review_result(
    const research::proto::EventSetReviewInput& input,
    const research::proto::EventSetReviewResult& v)
{
    validate_event_set_review_result(input, v);
    auto result = decode_event_set_review_input(input);
    Json reports = Json::array();
    for (int i = 0; i < v.members_size(); ++i)
        reports.push_back(decode_notice_review_result(input.members(i), v.members(i)));
    result["reports"] = std::move(reports);
    return result;
}

void validate_notice_source_observation(const research::proto::NoticeSourceObservation& value)
{
    validate_message(value);
    validate_notice_consumer_object(value.consumer());
    validate_notice_record(value.source());
    validate_notice_event_view(value.current());
    const auto& source = value.source();
    auto available = value.current().event().available_at_ms();
    if (value.current().has_latest_change())
        available =
            std::max(available, value.current().latest_change().published().available_at_ms());
    if (!source.has_published() || !source.has_event()
        || !same(source.event(), value.current().event())
        || value.consumer().owner().node_id() != source.reference().owner().node_id()
        || value.observed_at_ms() < available
        || value.observed_at_ms() < source.published().publication().available_at_ms())
        throw std::invalid_argument("notice source observation identity or time differs");
}

Json decode_notice_source_observation(const research::proto::NoticeSourceObservation& value)
{
    return {
        {"consumer", object(value.consumer())},
        {"source", object(value.source().published().reference())},
        {"current", decode_notice_event_view(value.current())},
        {"observed_at_ms", std::to_string(value.observed_at_ms())}
    };
}

void validate_notice_review_request(const research::proto::NoticeReviewRequest& v)
{
    validate_message(v);
    validate_id(v.source().owner().node_id());
    validate_id(v.source().owner().service_id());
    validate_id(v.source().id());
    digest(v.source().version());
    if (v.source().owner().domain() != "intelligence"
        || v.source().kind() != "intelligence_publication" || v.as_of_ms() <= 0
        || v.use_scope().empty() || v.use_scope().size() > 4096
        || v.use_scope().find('\0') != std::string::npos)
        throw std::invalid_argument("invalid fixed notice review request");
}

void validate_notice_review_input(const research::proto::NoticeReviewInput& v)
{
    validate_message(v);
    owner(v.owner());
    digest(v.program_artifact());
    validate_notice_review_request(v.request());
    validate_notice_published(v.publication());
    validate_notice_payload(v.content().payload());
    validate_notice_storage(v.content().receipt());
    const auto& publication = v.publication().publication();
    const auto& payload = v.content().payload();
    const auto body = payload.SerializeAsString();
    if (!same(v.request().source(), v.publication().reference())
        || !same(publication.storage(), v.content().receipt())
        || !same(publication.owner(), payload.owner())
        || !same(publication.candidate(), payload.candidate()) || publication.id() != payload.id()
        || body.size() != v.content().receipt().bytes()
        || sha256_bytes(body) != v.content().receipt().content().version()
        || v.content().receipt().stored_at_ms() < payload.verification().verified_at_ms()
        || v.owner().node_id() != publication.owner().node_id()
        || v.request().use_scope() != payload.input().candidate().use_scope())
        throw std::invalid_argument(
            "notice review source differs from its fixed publication or Data content");
}

research::proto::NoticeReviewInput make_notice_review_input(
    const resource::proto::Owner& owner,
    const research::proto::NoticeReviewRequest& request,
    const intelligence::proto::NoticePublished& publication,
    const intelligence::proto::NoticeStoredContent& content,
    const std::string& program)
{
    research::proto::NoticeReviewInput v;
    *v.mutable_owner() = owner;
    *v.mutable_request() = request;
    *v.mutable_publication() = publication;
    *v.mutable_content() = content;
    v.set_program_artifact(program);
    validate_notice_review_input(v);
    return v;
}

research::proto::NoticeReviewExecution notice_review_execution(
    const research::proto::NoticeReviewInput& v)
{
    validate_notice_review_input(v);
    research::proto::NoticeReviewExecution result;
    *result.mutable_owner() = v.owner();
    *result.mutable_request() = v.request();
    result.set_program_artifact(v.program_artifact());
    return result;
}

void validate_notice_review_execution(const research::proto::NoticeReviewExecution& v)
{
    validate_message(v);
    owner(v.owner());
    digest(v.program_artifact());
    validate_notice_review_request(v.request());
    if (v.owner().node_id() != v.request().source().owner().node_id())
        throw std::invalid_argument("cross-node notice review is unavailable");
}

void validate_notice_review_result(
    const research::proto::NoticeReviewInput& input,
    const research::proto::NoticeReviewResult& v)
{
    validate_notice_review_input(input);
    validate_message(v);
    const auto& payload = input.content().payload();
    const auto& original = payload.input().material().evidence();
    const auto& publication = input.publication().publication();
    if (!same(v.owner(), input.owner()) || !same(v.source(), input.request().source())
        || !same(v.content(), input.content().receipt().content())
        || v.program_artifact() != input.program_artifact()
        || v.as_of_ms() != input.request().as_of_ms()
        || v.use_scope() != input.request().use_scope()
        || v.contract() != payload.input().candidate().contract()
        || v.effective_day() != payload.assessment().effective_day()
        || !same(v.margin_ratio(), payload.assessment().margin_ratio())
        || v.planned() != payload.assessment().planned_marker_present()
        || v.has_source_published_at_ms() != original.has_source_published_at_ms()
        || v.source_published_at_ms() != original.source_published_at_ms()
        || v.acquired_at_ms() != original.acquired_at_ms()
        || v.processed_at_ms() != payload.processed_at_ms()
        || v.verified_at_ms() != payload.verification().verified_at_ms()
        || v.stored_at_ms() != publication.storage().stored_at_ms()
        || v.available_at_ms() != publication.available_at_ms()
        || v.published_by_cutoff() != (publication.available_at_ms() <= v.as_of_ms()))
        throw std::invalid_argument("notice review result differs from fixed evidence");
    if (v.strict_history_eligible()
        && (!v.published_by_cutoff()
            || payload.input().candidate().use_mode() != intelligence::proto::STRICT_HISTORY
            || !original.has_source_published_at_ms() || original.source_published_at_ms() <= 0
            || original.source_published_at_ms() > original.acquired_at_ms()))
        throw std::invalid_argument(
            "notice review cannot claim unsupported strict history eligibility");
    const std::set<std::string> known{
        "fixed_announcement_evidence_only",
        "no_prediction_validation",
        "source_publication_time_unknown",
        "source_publication_time_inconsistent",
        "post_hoc_publication",
        "publication_after_cutoff"
    };
    std::set<std::string> seen;
    for (const auto& s : v.limitations())
        if (!known.contains(s) || !seen.insert(s).second)
            throw std::invalid_argument("invalid notice review limitation");
    if (!seen.contains("fixed_announcement_evidence_only")
        || !seen.contains("no_prediction_validation")
        || (!v.published_by_cutoff() && !seen.contains("publication_after_cutoff"))
        || (payload.input().candidate().use_mode() != intelligence::proto::STRICT_HISTORY
            && !seen.contains("post_hoc_publication"))
        || (!original.has_source_published_at_ms()
            && !seen.contains("source_publication_time_unknown")))
        throw std::invalid_argument("notice review omits required evidence limitations");
}

Json decode_notice_review_input(const research::proto::NoticeReviewInput& v)
{
    validate_notice_review_input(v);
    const auto& payload = v.content().payload();
    return {
        {"owner",
            {{"node", v.owner().node_id()},
                {"service", v.owner().service_id()},
                {"domain", "research"}}},
        {"source", object(v.request().source())},
        {"content", object(v.content().receipt().content())},
        {"as_of_ms", std::to_string(v.request().as_of_ms())},
        {"use_scope", v.request().use_scope()},
        {"contract", payload.input().candidate().contract()},
        {"material", object(payload.input().material().reference())},
        {"verification_program", payload.verification().program_artifact()},
        {"program_artifact", v.program_artifact()}
    };
}

Json decode_notice_review_result(
    const research::proto::NoticeReviewInput& input,
    const research::proto::NoticeReviewResult& v)
{
    validate_notice_review_result(input, v);
    Json limits = Json::array();
    for (const auto& s : v.limitations())
        limits.push_back(s);
    return {
        {"owner",
            {{"node", v.owner().node_id()},
                {"service", v.owner().service_id()},
                {"domain", "research"}}},
        {"source", object(v.source())},
        {"content", object(v.content())},
        {"program_artifact", v.program_artifact()},
        {"as_of_ms", std::to_string(v.as_of_ms())},
        {"use_scope", v.use_scope()},
        {"contract", v.contract()},
        {"effective_day", v.effective_day()},
        {"margin_ratio", Decimal::from_raw(v.margin_ratio().units()).str()},
        {"planned", v.planned()},
        {"source_published_at_ms",
            v.has_source_published_at_ms() ? Json(std::to_string(v.source_published_at_ms()))
                                           : Json(nullptr)},
        {"acquired_at_ms", std::to_string(v.acquired_at_ms())},
        {"processed_at_ms", std::to_string(v.processed_at_ms())},
        {"verified_at_ms", std::to_string(v.verified_at_ms())},
        {"stored_at_ms", std::to_string(v.stored_at_ms())},
        {"available_at_ms", std::to_string(v.available_at_ms())},
        {"published_by_cutoff", v.published_by_cutoff()},
        {"strict_history_eligible", v.strict_history_eligible()},
        {"limitations", limits}
    };
}

} // namespace asterion::protocol
