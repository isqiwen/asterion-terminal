#include <asterion/protocol/intelligence.hpp>
#include <asterion/protocol/material.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/domain/trading_days.hpp>
#include <set>
#include <limits>
#include <algorithm>

namespace asterion::protocol {
namespace {

void digest(const std::string& value)
{
    if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("notice fixed identity is invalid");
}

void owner(const resource::proto::Owner& value, std::string_view domain)
{
    validate_id(value.node_id());
    validate_id(value.service_id());
    if (value.domain() != domain)
        throw std::invalid_argument("notice owner domain is invalid");
}

void reference(const resource::proto::Object& value, std::string_view kind)
{
    owner(value.owner(), "intelligence");
    validate_id(value.id());
    digest(value.version());
    if (value.kind() != kind)
        throw std::invalid_argument("notice reference kind is invalid");
}

void text(const std::string& value, std::size_t limit)
{
    if (value.empty() || value.size() > limit || value.find('\0') != std::string::npos)
        throw std::invalid_argument("notice decision text is missing or exceeds bounds");
}

bool same(const google::protobuf::Message& a, const google::protobuf::Message& b)
{
    return a.SerializeAsString() == b.SerializeAsString();
}

Json notice_object(const resource::proto::Object& p)
{
    return {
        {"node", p.owner().node_id()},
        {"service", p.owner().service_id()},
        {"domain", p.owner().domain()},
        {"kind", p.kind()},
        {"id", p.id()},
        {"version", p.version()}
    };
}

void verification(const intelligence::proto::NoticeVerification& value)
{
    validate_notice_review(value.review());
    digest(value.program_artifact());
    if (value.verified_at_ms() <= 0)
        throw std::invalid_argument("notice verification time is missing");
}

} // namespace

void validate_notice_delivery(const intelligence::proto::NoticeDelivery& value)
{
    validate_message(value);
    using namespace intelligence::proto;
    if (value.kind() < STATEMENT_PUBLISHED || value.kind() > EVENT_WITHDRAWN || !value.position()
        || value.position() > std::numeric_limits<std::int64_t>::max()
        || value.available_at_ms() <= 0)
        throw std::invalid_argument("notice delivery position, kind or time is invalid");
    reference(
        value.source(),
        value.kind() == STATEMENT_PUBLISHED ? "intelligence_publication" : "intelligence_change");
    reference(value.event(), "intelligence_event");
    reference(value.initial_publication(), "intelligence_publication");
    text(value.contract(), 256);
    text(value.rule_id(), 256);
    text(value.use_scope(), 4096);
    if (!same(value.source().owner(), value.event().owner())
        || !same(value.source().owner(), value.initial_publication().owner()))
        throw std::invalid_argument("notice delivery event belongs to another owner");
}

void validate_notice_delivery_query(const intelligence::proto::NoticeDeliveryQuery& value)
{
    validate_message(value);
    const auto& filter = value.filter();
    text(filter.use_scope(), 4096);
    if (filter.rule_id() != "contract_margin_announcement" || filter.contracts_size() < 1
        || filter.contracts_size() > 16 || filter.kinds_size() < 1 || filter.kinds_size() > 3
        || value.limit() > 64 || value.after() > std::numeric_limits<std::int64_t>::max()
        || (value.has_through()
            && (value.through() < value.after()
                || value.through() > std::numeric_limits<std::int64_t>::max())))
        throw std::invalid_argument(
            "notice delivery query requires bounded explicit conditions and positions");
    std::string previous;
    for (const auto& contract : filter.contracts()) {
        text(contract, 256);
        if (contract <= previous)
            throw std::invalid_argument("notice delivery contracts must be unique and sorted");
        previous = contract;
    }
    int last = 0;
    for (int kind : filter.kinds()) {
        if (kind < intelligence::proto::STATEMENT_PUBLISHED
            || kind > intelligence::proto::EVENT_WITHDRAWN || kind <= last)
            throw std::invalid_argument("notice delivery change kinds must be explicit and sorted");
        last = kind;
    }
}

bool notice_delivery_matches(
    const intelligence::proto::NoticeDeliveryFilter& filter,
    const intelligence::proto::NoticeDelivery& value)
{
    return (value.kind() != intelligence::proto::STATEMENT_PUBLISHED
               || filter.include_followup_statements()
               || same(value.source(), value.initial_publication()))
        && value.rule_id() == filter.rule_id() && value.use_scope() == filter.use_scope()
        && std::ranges::find(filter.contracts(), value.contract()) != filter.contracts().end()
        && std::ranges::find(filter.kinds(), value.kind()) != filter.kinds().end();
}

void validate_notice_delivery_page(const intelligence::proto::NoticeDeliveryPage& value)
{
    validate_message(value);
    owner(value.owner(), "intelligence");
    validate_notice_delivery_query(value.query());
    const auto& query = value.query();
    const auto limit = query.limit() ? query.limit() : 64U;
    if (value.head() > std::numeric_limits<std::int64_t>::max() || value.through() > value.head()
        || value.scanned_through() > value.through() || value.scanned_through() < query.after()
        || value.through() != (query.has_through() ? query.through() : value.head())
        || value.more() != (value.scanned_through() < value.through())
        || value.items_size() > static_cast<int>(limit))
        throw std::invalid_argument("notice delivery page coverage differs from its query");
    auto previous = query.after();
    for (const auto& item : value.items()) {
        validate_notice_delivery(item);
        if (!same(item.source().owner(), value.owner()) || item.position() <= previous
            || item.position() > value.scanned_through()
            || !notice_delivery_matches(query.filter(), item))
            throw std::invalid_argument(
                "notice delivery page changes differ from its fixed conditions");
        previous = item.position();
    }
}

void validate_notice_subscription_definition(
    const intelligence::proto::NoticeSubscriptionDefinition& value)
{
    validate_message(value);
    validate_id(value.id());
    text(value.name(), 256);
    intelligence::proto::NoticeDeliveryQuery query;
    *query.mutable_filter() = value.filter();
    query.set_after(value.start_after());
    validate_notice_delivery_query(query);
    if (value.consumer() != intelligence::proto::NoticeSubscriptionDefinition::APPLICATION_REMINDER)
        throw std::invalid_argument("notice subscription consumer is not implemented");
}

void validate_notice_subscription(const intelligence::proto::NoticeSubscriptionRecord& value)
{
    validate_message(value);
    validate_notice_subscription_definition(value.definition());
    reference(value.reference(), "intelligence_subscription");
    if (value.reference().id() != value.definition().id()
        || value.reference().version() != sha256_bytes(value.definition().SerializeAsString())
        || !value.revision() || value.revision() > std::numeric_limits<std::int64_t>::max()
        || value.confirmed_through() < value.definition().start_after()
        || value.confirmed_through() > std::numeric_limits<std::int64_t>::max()
        || value.paused_through() > std::numeric_limits<std::int64_t>::max()
        || (!value.active() && value.paused_through() < value.confirmed_through())
        || value.gap_through() > std::numeric_limits<std::int64_t>::max()
        || (value.gap_through() && value.gap_through() <= value.confirmed_through())
        || value.created_at_ms() <= 0 || value.updated_at_ms() < value.created_at_ms())
        throw std::invalid_argument("notice subscription identity or confirmed range differs");
    if (value.has_pending_delivery()) {
        reference(value.pending_delivery(), "intelligence_delivery");
        if (!same(value.pending_delivery().owner(), value.reference().owner()))
            throw std::invalid_argument(
                "notice subscription pending delivery belongs to another owner");
    }
}

void validate_notice_subscription_view(const intelligence::proto::NoticeSubscriptionView& value)
{
    validate_message(value);
    validate_notice_subscription(value.subscription());
    if (value.head() > std::numeric_limits<std::int64_t>::max()
        || value.head() < value.subscription().confirmed_through()
        || value.head() < value.subscription().paused_through()
        || value.head() < value.subscription().gap_through())
        throw std::invalid_argument(
            "notice subscription source head differs from durable progress");
}

void validate_notice_delivery_batch(const intelligence::proto::NoticeDeliveryBatch& value)
{
    validate_message(value);
    reference(value.reference(), "intelligence_delivery");
    const auto& payload = value.payload();
    const auto& request = payload.request();
    reference(request.subscription(), "intelligence_subscription");
    validate_id(request.operation_id());
    validate_notice_delivery_page(payload.page());
    if (value.reference().id() != request.operation_id()
        || value.reference().version() != sha256_bytes(payload.SerializeAsString())
        || !same(value.reference().owner(), request.subscription().owner())
        || !same(value.reference().owner(), payload.page().owner())
        || request.after() != payload.page().query().after()
        || !payload.page().query().has_through() || request.through() != payload.page().through()
        || payload.offered_at_ms() <= 0)
        throw std::invalid_argument("notice delivery batch differs from its fixed request");
    for (const auto& item : payload.page().items())
        if (item.available_at_ms() > payload.offered_at_ms())
            throw std::invalid_argument("notice delivery batch predates its published changes");
}

void validate_notice_reminder(const intelligence::proto::NoticeReminder& value)
{
    validate_message(value);
    reference(value.subscription(), "intelligence_subscription");
    validate_notice_delivery(value.change());
    if (!same(value.subscription().owner(), value.change().source().owner())
        || value.first_offered_at_ms() < value.change().available_at_ms())
        throw std::invalid_argument(
            "notice reminder differs from its source owner or availability");
}

void validate_notice_consumer_object(const resource::proto::Object& value)
{
    validate_message(value);
    owner(value.owner(), "research");
    validate_id(value.id());
    digest(value.version());
    if (value.kind() != "notice_review" && value.kind() != "event_set_review")
        throw std::invalid_argument("unsupported notice consumer kind");
}

std::string notice_consumer_key(const resource::proto::Object& value)
{
    validate_notice_consumer_object(value);
    auto key = value;
    key.clear_version();
    return sha256_bytes(key.SerializeAsString());
}

void validate_notice_consumer_binding(const intelligence::proto::NoticeConsumerBinding& value)
{
    validate_message(value);
    validate_notice_consumer_object(value.consumer());
    reference(
        value.source(),
        value.consumer().kind() == "event_set_review" ? "intelligence_set"
                                                      : "intelligence_publication");
    text(value.use_scope(), 4096);
    if (value.source().owner().node_id() != value.consumer().owner().node_id())
        throw std::invalid_argument("cross-node notice custody is unavailable");
}

void validate_notice_consumer_record(const intelligence::proto::NoticeConsumerRecord& value)
{
    validate_message(value);
    validate_notice_consumer_binding(value.binding());
    if (value.phase() < intelligence::proto::NoticeConsumerRecord::PREPARED
        || value.phase() > intelligence::proto::NoticeConsumerRecord::RELEASED
        || value.created_at_ms() <= 0 || value.updated_at_ms() < value.created_at_ms())
        throw std::invalid_argument("invalid notice custody state or time");
}

Json decode_notice_consumer_record(const intelligence::proto::NoticeConsumerRecord& value)
{
    validate_notice_consumer_record(value);
    const auto object = [](const resource::proto::Object& ref) {
        return Json{
            {"node", ref.owner().node_id()},
            {"service", ref.owner().service_id()},
            {"domain", ref.owner().domain()},
            {"kind", ref.kind()},
            {"id", ref.id()},
            {"version", ref.version()}
        };
    };
    return {
        {"source", object(value.binding().source())},
        {"consumer", object(value.binding().consumer())},
        {"use_scope", value.binding().use_scope()},
        {"phase",
            value.phase() == intelligence::proto::NoticeConsumerRecord::PREPARED ? "prepared"
                : value.phase() == intelligence::proto::NoticeConsumerRecord::CONFIRMED
                ? "confirmed"
                : "released"},
        {"created_at_ms", std::to_string(value.created_at_ms())},
        {"updated_at_ms", std::to_string(value.updated_at_ms())}
    };
}

void validate_notice_change_request(const intelligence::proto::NoticeChangeRequest& value)
{
    validate_message(value);
    validate_id(value.id());
    reference(value.event(), "intelligence_event");
    reference(
        value.expected_head(),
        value.expected_head().kind() == "intelligence_publication" ? "intelligence_publication"
                                                                   : "intelligence_change");
    if (!same(value.event().owner(), value.expected_head().owner()))
        throw std::invalid_argument("notice change head owner differs");
    if (value.action() == intelligence::proto::NoticeChangeRequest::CORRECT) {
        reference(value.replacement(), "intelligence_publication");
        if (!same(value.event().owner(), value.replacement().owner()))
            throw std::invalid_argument("notice correction replacement owner differs");
    } else if (value.action() != intelligence::proto::NoticeChangeRequest::WITHDRAW
        || value.has_replacement())
        throw std::invalid_argument("notice change action or replacement is invalid");
    owner(value.material().owner(), "data");
    validate_id(value.material().id());
    digest(value.material().version());
    text(value.evidence().text(), 8192);
    text(value.operator_name(), 256);
    text(value.reason(), 4096);
    if (value.material().kind() != "material_acquisition"
        || value.material().owner().node_id() != value.event().owner().node_id()
        || value.evidence().begin() >= value.evidence().end()
        || value.evidence().end() > max_material_bytes
        || value.evidence().end() - value.evidence().begin() != value.evidence().text().size()
        || !value.human_confirmed()
        || (value.has_effective_at_ms() && value.effective_at_ms() <= 0))
        throw std::invalid_argument("notice change lacks explicit human evidence");
}

void validate_notice_change_input(const intelligence::proto::NoticeChangeInput& value)
{
    validate_message(value);
    owner(value.owner(), "intelligence");
    validate_notice_change_request(value.request());
    validate_material_receipt(value.material());
    digest(value.program_artifact());
    if (!same(value.owner(), value.request().event().owner())
        || !same(value.material().reference(), value.request().material()))
        throw std::invalid_argument("notice change input identity differs");
}

void validate_notice_change_intent(const intelligence::proto::NoticeChangeIntent& value)
{
    validate_message(value);
    const auto& payload = value.payload();
    validate_notice_change_input(payload.input());
    reference(payload.affected_publication(), "intelligence_publication");
    reference(value.reference(), "intelligence_change_preparation");
    owner(value.data_owner(), "data");
    if (!same(value.reference().owner(), payload.input().owner())
        || !same(payload.affected_publication().owner(), payload.input().owner())
        || value.reference().id() != payload.input().request().id()
        || value.reference().version() != sha256_bytes(payload.SerializeAsString())
        || !same(value.data_owner(), payload.input().material().reference().owner())
        || payload.reviewed_at_ms() < payload.input().material().evidence().acquired_at_ms()
        || payload.reviewed_at_ms() <= 0 || value.prepared_at_ms() < payload.reviewed_at_ms())
        throw std::invalid_argument("notice change intent differs from fixed evidence");
}

void validate_notice_change_record(const intelligence::proto::NoticeChangeRecord& value)
{
    validate_message(value);
    validate_notice_change_intent(value.intent());
    if (!value.has_published())
        return;
    const auto& published = value.published();
    reference(published.reference(), "intelligence_change");
    validate_notice_storage(published.storage());
    auto identity = published;
    identity.clear_reference();
    if (!same(published.reference().owner(), value.intent().reference().owner())
        || published.reference().id() != value.intent().reference().id()
        || published.reference().version() != sha256_bytes(identity.SerializeAsString())
        || !same(published.preparation(), value.intent().reference())
        || !same(published.storage().origin(), value.intent().reference())
        || !same(published.storage().content().owner(), value.intent().data_owner())
        || published.storage().bytes() != value.intent().payload().ByteSizeLong()
        || published.storage().stored_at_ms() < value.intent().prepared_at_ms()
        || published.available_at_ms() < published.storage().stored_at_ms()
        || published.sequence() == 0)
        throw std::invalid_argument("notice change publication lacks exact Data confirmation");
}

void validate_notice_event(const intelligence::proto::NoticeEvent& value)
{
    validate_message(value);
    reference(value.reference(), "intelligence_event");
    reference(value.initial_publication(), "intelligence_publication");
    text(value.contract(), 128);
    text(value.use_scope(), 4096);
    text(value.rule_id(), 128);
    auto definition = value;
    definition.clear_reference();
    if (!same(value.reference().owner(), value.initial_publication().owner())
        || value.available_at_ms() <= 0
        || value.reference().version() != sha256_bytes(definition.SerializeAsString()))
        throw std::invalid_argument("notice event definition or identity differs");
}

void validate_notice_set_request(const intelligence::proto::NoticeSetRequest& value)
{
    validate_message(value);
    validate_id(value.id());
    text(value.name(), 256);
    digest(value.program_artifact());
    owner(value.data_owner(), "data");
    const auto& query = value.query();
    text(query.use_scope(), 4096);
    if (query.events_size() < 1 || query.events_size() > 16 || query.as_of_ms() <= 0
        || (query.use_mode() != intelligence::proto::POST_HOC_RESEARCH
            && query.use_mode() != intelligence::proto::STRICT_HISTORY))
        throw std::invalid_argument(
            "event set requires bounded explicit events, use mode and cutoff");
    std::string previous;
    for (const auto& event : query.events()) {
        reference(event, "intelligence_event");
        if (!same(event.owner(), query.events(0).owner()) || event.id() <= previous
            || event.owner().node_id() != value.data_owner().node_id())
            throw std::invalid_argument("event set event order, owner or target differs");
        previous = event.id();
    }
}

void validate_notice_set_payload(const intelligence::proto::NoticeSetPayload& value)
{
    validate_message(value);
    validate_notice_set_request(value.request());
    owner(value.owner(), "intelligence");
    const auto& query = value.request().query();
    if (!same(value.owner(), query.events(0).owner())
        || value.members_size() + value.exclusions_size() != query.events_size()
        || value.ByteSizeLong() > 1024 * 1024)
        throw std::invalid_argument("event set ownership, coverage or size differs");
    std::set<std::string> wanted, included;
    for (const auto& event : query.events())
        wanted.insert(event.SerializeAsString());
    const auto include = [&](const resource::proto::Object& event) {
        if (!wanted.contains(event.SerializeAsString())
            || !included.insert(event.SerializeAsString()).second)
            throw std::invalid_argument("event set repeats or substitutes selected events");
    };
    const auto limitation = [&](const intelligence::proto::NoticeSetLimitation& item) {
        if (item.source().kind() != "intelligence_event"
            && item.source().kind() != "intelligence_publication"
            && item.source().kind() != "intelligence_change")
            throw std::invalid_argument("event set limitation evidence kind is invalid");
        reference(item.source(), item.source().kind());
        text(item.reason(), 256);
        if (!same(item.source().owner(), value.owner()))
            throw std::invalid_argument("event set limitation belongs to another owner");
    };
    for (const auto& member : value.members()) {
        validate_notice_event_view(member.event());
        validate_notice_published(member.publication());
        const auto& view = member.event();
        include(view.event().reference());
        if (view.withdrawn() || view.has_pending_change()
            || !same(view.selected_publication(), member.publication().reference())
            || view.event().use_scope() != query.use_scope()
            || view.event().available_at_ms() > query.as_of_ms()
            || member.publication().publication().available_at_ms() > query.as_of_ms()
            || (view.has_latest_change()
                && (view.latest_change().published().available_at_ms() > query.as_of_ms()
                    || !same(
                        view.latest_change().published().storage().content().owner(),
                        value.request().data_owner())))
            || !same(
                member.publication().publication().storage().content().owner(),
                value.request().data_owner())
            || member.limitations_size() > 64)
            throw std::invalid_argument("event set member differs from fixed historical selection");
        for (const auto& item : member.limitations())
            limitation(item);
    }
    for (const auto& excluded : value.exclusions()) {
        include(excluded.event());
        if (excluded.reasons_size() < 1 || excluded.reasons_size() > 64)
            throw std::invalid_argument("event set exclusion requires explicit reasons");
        for (const auto& item : excluded.reasons())
            limitation(item);
    }
}

void validate_notice_set_intent(const intelligence::proto::NoticeSetIntent& value)
{
    validate_message(value);
    validate_notice_set_payload(value.payload());
    reference(value.reference(), "intelligence_set_preparation");
    if (!same(value.reference().owner(), value.payload().owner())
        || value.reference().id() != value.payload().request().id()
        || value.reference().version() != sha256_bytes(value.payload().SerializeAsString())
        || !same(value.data_owner(), value.payload().request().data_owner())
        || value.prepared_at_ms() < value.payload().request().query().as_of_ms()
        || value.payload().members().empty())
        throw std::invalid_argument("event set preparation lacks fixed nonempty content");
}

void validate_notice_set_record(const intelligence::proto::NoticeSetRecord& value)
{
    validate_message(value);
    validate_notice_set_intent(value.intent());
    if (!value.has_published())
        return;
    const auto& published = value.published();
    reference(published.reference(), "intelligence_set");
    validate_notice_storage(published.storage());
    auto identity = published;
    identity.clear_reference();
    if (!same(published.reference().owner(), value.intent().reference().owner())
        || published.reference().id() != value.intent().reference().id()
        || published.reference().version() != sha256_bytes(identity.SerializeAsString())
        || !same(published.preparation(), value.intent().reference())
        || !same(published.storage().origin(), value.intent().reference())
        || !same(published.storage().content().owner(), value.intent().data_owner())
        || published.storage().bytes() != value.intent().payload().ByteSizeLong()
        || published.storage().stored_at_ms() < value.intent().prepared_at_ms()
        || published.available_at_ms() < published.storage().stored_at_ms())
        throw std::invalid_argument("event set publication lacks exact Data confirmation");
}

void validate_notice_event_view(const intelligence::proto::NoticeEventView& view)
{
    validate_message(view);
    validate_notice_event(view.event());
    reference(
        view.head(),
        view.has_latest_change() ? "intelligence_change" : "intelligence_publication");
    reference(view.selected_publication(), "intelligence_publication");
    if (!same(view.head().owner(), view.event().reference().owner())
        || !same(view.selected_publication().owner(), view.event().reference().owner()))
        throw std::invalid_argument("notice event view owner differs");
    if (view.has_latest_change()) {
        const auto& change = view.latest_change();
        validate_notice_change_record(change);
        const auto& request = change.intent().payload().input().request();
        const bool withdrawn =
            request.action() == intelligence::proto::NoticeChangeRequest::WITHDRAW;
        const auto& selected =
            withdrawn ? change.intent().payload().affected_publication() : request.replacement();
        if (!change.has_published() || !same(change.published().reference(), view.head())
            || !same(request.event(), view.event().reference()) || view.withdrawn() != withdrawn
            || !same(selected, view.selected_publication())
            || change.published().available_at_ms() < view.event().available_at_ms())
            throw std::invalid_argument("notice event current change differs");
    } else if (view.withdrawn() || !same(view.head(), view.event().initial_publication())
        || !same(view.selected_publication(), view.head()))
        throw std::invalid_argument("notice initial event view differs");
    if (view.has_pending_change()) {
        const auto& pending = view.pending_change();
        validate_notice_change_record(pending);
        const auto& request = pending.intent().payload().input().request();
        if (view.withdrawn() || pending.has_published()
            || !same(request.event(), view.event().reference())
            || !same(request.expected_head(), view.head()))
            throw std::invalid_argument("notice pending change view differs");
    }
}

Json decode_notice_event(const intelligence::proto::NoticeEvent& event)
{
    return {
        {"reference", notice_object(event.reference())},
        {"initial_publication", notice_object(event.initial_publication())},
        {"contract", event.contract()},
        {"use_scope", event.use_scope()},
        {"rule_id", event.rule_id()},
        {"available_at_ms", std::to_string(event.available_at_ms())}
    };
}

Json decode_notice_change(const intelligence::proto::NoticeChangeRecord& record)
{
    const auto& intent = record.intent();
    const auto& input = intent.payload().input();
    const auto& request = input.request();
    Json published = nullptr;
    if (record.has_published()) {
        const auto& value = record.published();
        published = {
            {"reference", notice_object(value.reference())},
            {"content", notice_object(value.storage().content())},
            {"stored_at_ms", std::to_string(value.storage().stored_at_ms())},
            {"available_at_ms", std::to_string(value.available_at_ms())},
            {"sequence", std::to_string(value.sequence())}
        };
    }
    return {
        {"reference", notice_object(intent.reference())},
        {"affected_publication", notice_object(intent.payload().affected_publication())},
        {"event", notice_object(request.event())},
        {"expected_head", notice_object(request.expected_head())},
        {"action",
            request.action() == intelligence::proto::NoticeChangeRequest::CORRECT ? "correct"
                                                                                  : "withdraw"},
        {"replacement",
            request.has_replacement() ? notice_object(request.replacement()) : Json(nullptr)},
        {"material", notice_object(request.material())},
        {"evidence",
            {{"begin", request.evidence().begin()},
                {"end", request.evidence().end()},
                {"text", request.evidence().text()}}},
        {"operator_name", request.operator_name()},
        {"reason", request.reason()},
        {"effective_at_ms",
            request.has_effective_at_ms() ? Json(std::to_string(request.effective_at_ms()))
                                          : Json(nullptr)},
        {"reviewed_at_ms", std::to_string(intent.payload().reviewed_at_ms())},
        {"prepared_at_ms", std::to_string(intent.prepared_at_ms())},
        {"program_artifact", input.program_artifact()},
        {"published", published}
    };
}

Json decode_notice_event_view(const intelligence::proto::NoticeEventView& value)
{
    return {
        {"event", decode_notice_event(value.event())},
        {"head", notice_object(value.head())},
        {"selected_publication", notice_object(value.selected_publication())},
        {"withdrawn", value.withdrawn()},
        {"latest_change",
            value.has_latest_change() ? decode_notice_change(value.latest_change())
                                      : Json(nullptr)},
        {"pending_change",
            value.has_pending_change() ? decode_notice_change(value.pending_change())
                                       : Json(nullptr)}
    };
}

void validate_notice_candidate(const intelligence::proto::NoticeCandidate& value)
{
    validate_message(value);
    validate_id(value.id());
    if (value.has_event())
        reference(value.event(), "intelligence_event");
    const auto& material = value.material();
    validate_id(material.owner().node_id());
    validate_id(material.owner().service_id());
    validate_id(material.id());
    if (material.owner().domain() != "data" || material.kind() != "material_acquisition"
        || material.version().size() != 64
        || material.version().find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("notice material identity is invalid");
    if (value.ByteSizeLong() > 65536 || value.contract().size() > 128
        || value.rule_id().size() > 128 || value.use_scope().size() > 4096
        || !intelligence::proto::NoticeUseMode_IsValid(value.use_mode()))
        throw std::invalid_argument("notice candidate exceeds contract bounds");
    const auto& dictionary = value.dictionary();
    if (!dictionary.snapshot().empty()
        && (dictionary.snapshot().size() != 64
            || dictionary.snapshot().find_first_not_of("0123456789abcdef") != std::string::npos))
        throw std::invalid_argument("notice dictionary snapshot is invalid");
    for (const auto* text :
        {&dictionary.contracts_source(), &dictionary.exchange(), &dictionary.product()})
        if (!text->empty())
            validate_id(*text);
    for (const auto* span :
        {&value.subject(),
            &value.parameter(),
            &value.effective_day(),
            &value.percentage(),
            &value.modality()})
        if (span->text().size() > 8192 || span->begin() > max_material_bytes
            || span->end() > max_material_bytes)
            throw std::invalid_argument("notice evidence range exceeds bounds");
}

void validate_notice_input(const intelligence::proto::NoticeInput& value)
{
    validate_message(value);
    owner(value.owner(), "intelligence");
    validate_notice_candidate(value.candidate());
    validate_material_receipt(value.material());
    digest(value.program_artifact());
    if (value.ByteSizeLong() > 131072
        || !same(value.material().reference(), value.candidate().material())
        || value.owner().node_id() != value.material().reference().owner().node_id())
        throw std::invalid_argument("notice input source differs from its fixed material");
    if (value.has_dictionary_reference()) {
        const auto& ref = value.dictionary_reference();
        owner(ref.owner(), "data");
        digest(ref.version());
        if (ref.kind() != "table_snapshot" || ref.id() != ref.version()
            || ref.id() != value.candidate().dictionary().snapshot()
            || !same(ref.owner(), value.material().reference().owner()))
            throw std::invalid_argument("notice dictionary differs from its fixed Data snapshot");
    }
    if (value.has_dictionary_entry()
        && (!value.has_dictionary_reference()
            || value.dictionary_entry().contract_id() != value.candidate().contract()))
        throw std::invalid_argument("notice dictionary entry differs from its selected contract");
}

void validate_notice_assessment(const intelligence::proto::NoticeAssessment& value)
{
    validate_message(value);
    const std::set<std::string> required{
        "rule",
        "use_scope",
        "subject_evidence",
        "parameter_evidence",
        "effective_day_evidence",
        "percentage_evidence",
        "modality_evidence",
        "subject_mapping",
        "parameter",
        "effective_day",
        "percentage",
        "modality",
        "availability_mode",
        "material_readability",
        "contract_lifetime"
    };
    std::set<std::string> fields;
    bool passed = true;
    if (value.checks_size() < 1 || value.checks_size() > 32 || value.limitations_size() > 32)
        throw std::invalid_argument("notice assessment exceeds bounds");
    for (const auto& check : value.checks()) {
        if (!required.contains(check.field()) || !fields.insert(check.field()).second
            || check.state() < intelligence::proto::PASS
            || check.state() > intelligence::proto::UNKNOWN)
            throw std::invalid_argument("notice assessment check is invalid");
        validate_id(check.reason());
        passed = passed && check.state() == intelligence::proto::PASS;
    }
    for (const auto& reason : value.limitations())
        validate_id(reason);
    if (!value.effective_day().empty())
        (void)parse_trading_date(value.effective_day());
    const bool ratio = value.has_margin_ratio() && value.margin_ratio().units() > 0
        && value.margin_ratio().units() <= Decimal::parse("1").raw();
    if (value.has_margin_ratio() && !ratio)
        throw std::invalid_argument("notice margin ratio is invalid");
    const bool ready = passed && fields == required && ratio && !value.effective_day().empty()
        && value.planned_marker_present();
    if (value.ready_for_human_review() != ready)
        throw std::invalid_argument("notice assessment readiness differs from its checks");
}

void validate_notice_review(const intelligence::proto::NoticeReview& value)
{
    validate_message(value);
    validate_id(value.id());
    reference(value.candidate(), "intelligence_candidate");
    text(value.operator_name(), 256);
    text(value.reason(), 4096);
    const std::set<std::string>
        required{"subject", "time", "quantity_unit", "modality_context", "source_use", "conflicts"};
    std::set<std::string> fields;
    bool passed = true;
    if ((value.action() != intelligence::proto::NoticeReview::APPROVE
            && value.action() != intelligence::proto::NoticeReview::REJECT)
        || value.checks_size() > 6 || value.ByteSizeLong() > 65536)
        throw std::invalid_argument("notice human review action or size is invalid");
    for (const auto& check : value.checks()) {
        if (!required.contains(check.field()) || !fields.insert(check.field()).second
            || check.state() < intelligence::proto::PASS
            || check.state() > intelligence::proto::UNKNOWN || check.reason().size() > 4096)
            throw std::invalid_argument("notice human review field is invalid");
        passed = passed && check.state() == intelligence::proto::PASS;
    }
    if (value.action() == intelligence::proto::NoticeReview::APPROVE
        && (!passed || fields != required))
        throw std::invalid_argument("notice approval requires every explicit human check");
}

void validate_notice_payload(const intelligence::proto::NoticePayload& value)
{
    validate_message(value);
    owner(value.owner(), "intelligence");
    validate_id(value.id());
    validate_notice_input(value.input());
    validate_notice_assessment(value.assessment());
    verification(value.verification());
    reference(value.candidate(), "intelligence_candidate");
    text(value.publication_reason(), 4096);
    if (value.ByteSizeLong() > 1024 * 1024 || !same(value.owner(), value.input().owner())
        || !same(value.owner(), value.candidate().owner())
        || value.candidate().id() != value.input().candidate().id()
        || value.candidate().version() != sha256_bytes(value.input().SerializeAsString())
        || !same(value.verification().review().candidate(), value.candidate())
        || value.verification().review().action() != intelligence::proto::NoticeReview::APPROVE
        || value.verification().program_artifact() != value.input().program_artifact()
        || !value.assessment().ready_for_human_review()
        || value.processed_at_ms() < value.input().material().evidence().acquired_at_ms()
        || value.processed_at_ms() <= 0
        || value.verification().verified_at_ms() < value.processed_at_ms())
        throw std::invalid_argument("notice payload lacks fixed verified publication evidence");
    const auto& candidate = value.input().candidate();
    const auto& material = value.input().material().evidence();
    if (candidate.use_scope() != material.request().use_scope()
        || candidate.use_mode() == intelligence::proto::UNSPECIFIED_USE
        || !value.input().has_dictionary_entry() || !value.input().has_dictionary_reference())
        throw std::invalid_argument("notice publication scope or dictionary is incomplete");
    if (candidate.use_mode() == intelligence::proto::STRICT_HISTORY
        && (!material.has_source_published_at_ms()
            || material.source_published_at_ms() > material.acquired_at_ms()))
        throw std::invalid_argument("notice strict history lacks source publication evidence");
}

void validate_notice_intent(const intelligence::proto::NoticePublicationIntent& value)
{
    validate_message(value);
    validate_notice_payload(value.payload());
    reference(value.reference(), "intelligence_preparation");
    owner(value.data_owner(), "data");
    if (!same(value.reference().owner(), value.payload().owner())
        || value.reference().id() != value.payload().id()
        || value.reference().version() != sha256_bytes(value.payload().SerializeAsString())
        || !same(value.data_owner(), value.payload().input().material().reference().owner())
        || value.prepared_at_ms() < value.payload().verification().verified_at_ms())
        throw std::invalid_argument("notice publication intent differs from its fixed payload");
}

void validate_notice_storage(const intelligence::proto::NoticeStorageReceipt& value)
{
    validate_message(value);
    owner(value.content().owner(), "data");
    const bool change = value.origin().kind() == "intelligence_change_preparation";
    const bool set = value.origin().kind() == "intelligence_set_preparation";
    reference(
        value.origin(),
        set ? "intelligence_set_preparation"
            : change ? "intelligence_change_preparation"
                     : "intelligence_preparation");
    digest(value.content().version());
    if (value.content().kind()
            != (set ? "intelligence_set_content"
                    : change ? "intelligence_change_content"
                             : "intelligence_content")
        || value.content().id() != value.content().version()
        || value.content().version() != value.origin().version()
        || value.content().owner().node_id() != value.origin().owner().node_id()
        || value.bytes() == 0 || value.bytes() > 1024 * 1024 || value.stored_at_ms() <= 0)
        throw std::invalid_argument("notice storage receipt differs from its prepared content");
}

void validate_notice_published(const intelligence::proto::NoticePublished& value)
{
    validate_message(value);
    reference(value.reference(), "intelligence_publication");
    const auto& record = value.publication();
    owner(record.owner(), "intelligence");
    validate_id(record.id());
    reference(record.candidate(), "intelligence_candidate");
    reference(record.preparation(), "intelligence_preparation");
    validate_notice_storage(record.storage());
    if (!same(record.owner(), value.reference().owner())
        || !same(record.owner(), record.candidate().owner())
        || !same(record.owner(), record.preparation().owner())
        || value.reference().id() != record.id() || record.preparation().id() != record.id()
        || value.reference().version() != sha256_bytes(record.SerializeAsString())
        || !same(record.storage().origin(), record.preparation())
        || record.available_at_ms() < record.storage().stored_at_ms())
        throw std::invalid_argument("notice publication identity or availability time is invalid");
}

void validate_notice_record(const intelligence::proto::NoticeRecord& value)
{
    validate_message(value);
    reference(value.reference(), "intelligence_candidate");
    validate_notice_input(value.input());
    validate_notice_assessment(value.assessment());
    if (!same(value.reference().owner(), value.input().owner())
        || value.reference().id() != value.input().candidate().id()
        || value.reference().version() != sha256_bytes(value.input().SerializeAsString())
        || value.created_at_ms() < value.input().material().evidence().acquired_at_ms()
        || value.created_at_ms() <= 0 || value.updated_at_ms() < value.created_at_ms()
        || value.state() < intelligence::proto::PREPARING
        || value.state() > intelligence::proto::ABANDONED)
        throw std::invalid_argument("notice candidate record is invalid");
    const bool reviewed = value.state() == intelligence::proto::VERIFIED
        || value.state() == intelligence::proto::PUBLISHING
        || value.state() == intelligence::proto::PUBLISHED
        || value.state() == intelligence::proto::REJECTED;
    const bool publishing = value.state() == intelligence::proto::PUBLISHING
        || value.state() == intelligence::proto::PUBLISHED;
    if (reviewed != value.has_verification() || publishing != value.has_intent()
        || (value.state() == intelligence::proto::PUBLISHED) != value.has_published())
        throw std::invalid_argument("notice state lacks its durable decision");
    if (reviewed) {
        verification(value.verification());
        if (!same(value.verification().review().candidate(), value.reference())
            || value.verification().program_artifact() != value.input().program_artifact()
            || value.verification().verified_at_ms() < value.created_at_ms()
            || value.verification().verified_at_ms() > value.updated_at_ms()
            || (value.state() == intelligence::proto::REJECTED)
                != (value.verification().review().action()
                    == intelligence::proto::NoticeReview::REJECT))
            throw std::invalid_argument("notice review does not belong to its candidate");
    }
    if (publishing) {
        validate_notice_intent(value.intent());
        if (!same(value.intent().payload().candidate(), value.reference())
            || !same(value.intent().payload().input(), value.input())
            || !same(value.intent().payload().assessment(), value.assessment())
            || !same(value.intent().payload().verification(), value.verification())
            || value.intent().payload().processed_at_ms() != value.created_at_ms()
            || value.intent().prepared_at_ms() > value.updated_at_ms())
            throw std::invalid_argument(
                "notice publication intent does not belong to its candidate");
    }
    if (value.has_event()) {
        validate_notice_event(value.event());
        const auto& candidate = value.input().candidate();
        if (!value.has_published()
            || !same(value.event().reference().owner(), value.reference().owner())
            || value.event().contract() != candidate.contract()
            || value.event().use_scope() != candidate.use_scope()
            || value.event().rule_id() != candidate.rule_id()
            || value.event().available_at_ms() > value.published().publication().available_at_ms()
            || (candidate.has_event()
                    ? !same(candidate.event(), value.event().reference())
                    : !same(value.event().initial_publication(), value.published().reference())))
            throw std::invalid_argument("notice statement event association differs");
    }
    if (value.has_published()) {
        validate_notice_published(value.published());
        const auto& published = value.published().publication();
        if (!same(published.candidate(), value.reference())
            || !same(published.preparation(), value.intent().reference())
            || !same(published.storage().content().owner(), value.intent().data_owner())
            || published.storage().bytes() != value.intent().payload().ByteSizeLong()
            || published.storage().stored_at_ms() < value.intent().prepared_at_ms()
            || published.available_at_ms() > value.updated_at_ms())
            throw std::invalid_argument(
                "notice publication storage confirmation differs from its intent");
    }
}

} // namespace asterion::protocol
