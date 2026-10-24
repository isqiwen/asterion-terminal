#include "application_impl.hpp"
#include "intelligence_call.hpp"
#include <asterion/protocol/intelligence.hpp>
#include <asterion/protocol/material.hpp>
#include <charconv>
#include <limits>

namespace asterion::terminal {
namespace ip = intelligence::proto;

namespace {

resource::proto::Owner notice_owner(const Json& p)
{
    require_fields(p, {"node", "service", "domain"});
    resource::proto::Owner result;
    result.set_node_id(p.at("node").get<std::string>());
    result.set_service_id(p.at("service").get<std::string>());
    result.set_domain(p.at("domain").get<std::string>());
    return result;
}

resource::proto::Object notice_object(const Json& p)
{
    require_fields(p, {"node", "service", "domain", "kind", "id", "version"});
    resource::proto::Object result;
    *result.mutable_owner() = notice_owner(
        {{"node", p.at("node")}, {"service", p.at("service")}, {"domain", p.at("domain")}});
    result.set_kind(p.at("kind").get<std::string>());
    result.set_id(p.at("id").get<std::string>());
    result.set_version(p.at("version").get<std::string>());
    return result;
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

ip::NoticeCandidate notice_candidate(const Json& p)
{
    auto fields = p;
    fields.erase("event");
    require_fields(
        fields,
        {"id",
            "material",
            "dictionary",
            "contract",
            "evidence",
            "rule_id",
            "use_scope",
            "use_mode"});
    ip::NoticeCandidate c;
    if (p.contains("event") && !p.at("event").is_null())
        *c.mutable_event() = notice_object(p.at("event"));
    c.set_id(p.at("id").get<std::string>());
    *c.mutable_material() = notice_object(p.at("material"));
    const auto& d = p.at("dictionary");
    require_fields(d, {"snapshot", "contracts_source", "exchange", "product"});
    c.mutable_dictionary()->set_snapshot(d.at("snapshot").get<std::string>());
    c.mutable_dictionary()->set_contracts_source(d.at("contracts_source").get<std::string>());
    c.mutable_dictionary()->set_exchange(d.at("exchange").get<std::string>());
    c.mutable_dictionary()->set_product(d.at("product").get<std::string>());
    c.set_contract(p.at("contract").get<std::string>());
    const auto& e = p.at("evidence");
    require_fields(e, {"subject", "parameter", "effective_day", "percentage", "modality"});
    const auto span = [&](ip::EvidenceSpan* s, const char* name) {
        const auto& v = e.at(name);
        require_fields(v, {"begin", "end", "text"});
        if (!v.at("begin").is_number_unsigned() || !v.at("end").is_number_unsigned())
            throw std::invalid_argument("notice evidence offsets must be nonnegative integers");
        s->set_begin(v.at("begin").get<std::uint64_t>());
        s->set_end(v.at("end").get<std::uint64_t>());
        s->set_text(v.at("text").get<std::string>());
    };
    span(c.mutable_subject(), "subject");
    span(c.mutable_parameter(), "parameter");
    span(c.mutable_effective_day(), "effective_day");
    span(c.mutable_percentage(), "percentage");
    span(c.mutable_modality(), "modality");
    c.set_rule_id(p.at("rule_id").get<std::string>());
    c.set_use_scope(p.at("use_scope").get<std::string>());
    const auto mode = p.at("use_mode").get<std::string>();
    if (mode != "post_hoc_research" && mode != "strict_history")
        throw std::invalid_argument("invalid notice use mode");
    c.set_use_mode(mode == "strict_history" ? ip::STRICT_HISTORY : ip::POST_HOC_RESEARCH);
    protocol::validate_notice_candidate(c);
    return c;
}

std::int64_t notice_time(const Json& value)
{
    const auto text = value.get<std::string>();
    std::int64_t result = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size() || result <= 0)
        throw std::invalid_argument("notice timestamp must be positive decimal milliseconds");
    return result;
}

ip::NoticeChangeRequest notice_change_request(const Json& p)
{
    require_fields(
        p,
        {"id",
            "event",
            "expected_head",
            "action",
            "replacement",
            "material",
            "evidence",
            "operator_name",
            "reason",
            "human_confirmed",
            "effective_at_ms"});
    ip::NoticeChangeRequest request;
    request.set_id(p.at("id").get<std::string>());
    *request.mutable_event() = notice_object(p.at("event"));
    *request.mutable_expected_head() = notice_object(p.at("expected_head"));
    *request.mutable_material() = notice_object(p.at("material"));
    const auto action = p.at("action").get<std::string>();
    if (action != "correct" && action != "withdraw")
        throw std::invalid_argument("notice change action is invalid");
    request.set_action(
        action == "correct" ? ip::NoticeChangeRequest::CORRECT : ip::NoticeChangeRequest::WITHDRAW);
    if (!p.at("replacement").is_null())
        *request.mutable_replacement() = notice_object(p.at("replacement"));
    const auto& quote = p.at("evidence");
    require_fields(quote, {"begin", "end", "text"});
    if (!quote.at("begin").is_number_unsigned() || !quote.at("end").is_number_unsigned())
        throw std::invalid_argument("notice evidence offsets must be nonnegative integers");
    request.mutable_evidence()->set_begin(quote.at("begin").get<std::uint64_t>());
    request.mutable_evidence()->set_end(quote.at("end").get<std::uint64_t>());
    request.mutable_evidence()->set_text(quote.at("text").get<std::string>());
    request.set_operator_name(p.at("operator_name").get<std::string>());
    request.set_reason(p.at("reason").get<std::string>());
    request.set_human_confirmed(p.at("human_confirmed").get<bool>());
    if (!p.at("effective_at_ms").is_null())
        request.set_effective_at_ms(notice_time(p.at("effective_at_ms")));
    protocol::validate_notice_change_request(request);
    return request;
}

Json notice_record(const ip::NoticeRecord& p)
{
    Json checks = Json::array();
    for (const auto& c : p.assessment().checks())
        checks.push_back(
            {{"field", c.field()},
                {"state",
                    c.state() == ip::PASS          ? "pass"
                        : c.state() == ip::BLOCKED ? "blocked"
                                                   : "unknown"},
                {"reason", c.reason()}});
    const auto& candidate = p.input().candidate();
    Json evidence = Json::object();
    const auto span = [&](const char* key, const ip::EvidenceSpan& v) {
        evidence[key] = {{"begin", v.begin()}, {"end", v.end()}, {"text", v.text()}};
    };
    span("subject", candidate.subject());
    span("parameter", candidate.parameter());
    span("effective_day", candidate.effective_day());
    span("percentage", candidate.percentage());
    span("modality", candidate.modality());
    Json published = nullptr;
    if (p.has_published()) {
        const auto& publication = p.published().publication();
        published = {
            {"reference", notice_object(p.published().reference())},
            {"preparation", notice_object(publication.preparation())},
            {"content", notice_object(publication.storage().content())},
            {"stored_at_ms", std::to_string(publication.storage().stored_at_ms())},
            {"available_at_ms", std::to_string(publication.available_at_ms())}
        };
    }
    Json verification = nullptr;
    if (p.has_verification())
        verification = {
            {"id", p.verification().review().id()},
            {"operator_name", p.verification().review().operator_name()},
            {"reason", p.verification().review().reason()},
            {"approved", p.verification().review().action() == ip::NoticeReview::APPROVE},
            {"verified_at_ms", std::to_string(p.verification().verified_at_ms())}
        };
    const char* state = p.state() == ip::PREPARING ? "preparing"
        : p.state() == ip::DRAFT                   ? "draft"
        : p.state() == ip::VERIFIED                ? "verified"
        : p.state() == ip::PUBLISHING              ? "publishing"
        : p.state() == ip::PUBLISHED               ? "published"
        : p.state() == ip::REJECTED                ? "rejected"
                                                   : "abandoned";
    return {
        {"reference", notice_object(p.reference())},
        {"state", state},
        {"material", protocol::decode_material_receipt(p.input().material())},
        {"dictionary_reference",
            p.input().has_dictionary_reference() ? notice_object(p.input().dictionary_reference())
                                                 : Json(nullptr)},
        {"contract", candidate.contract()},
        {"evidence", evidence},
        {"rule_id", candidate.rule_id()},
        {"use_scope", candidate.use_scope()},
        {"use_mode",
            candidate.use_mode() == ip::STRICT_HISTORY ? "strict_history" : "post_hoc_research"},
        {"program_artifact", p.input().program_artifact()},
        {"created_at_ms", std::to_string(p.created_at_ms())},
        {"updated_at_ms", std::to_string(p.updated_at_ms())},
        {"assessment",
            {{"checks", checks},
                {"limitations", Json(p.assessment().limitations())},
                {"ready_for_human_review", p.assessment().ready_for_human_review()},
                {"effective_day", p.assessment().effective_day()},
                {"margin_ratio",
                    p.assessment().has_margin_ratio()
                        ? Json(Decimal::from_raw(p.assessment().margin_ratio().units()).str())
                        : Json(nullptr)}}},
        {"verification", verification},
        {"publication_request",
            p.has_intent()
                ? Json{{"id", p.intent().payload().id()}, {"reason", p.intent().payload().publication_reason()}}
                : Json(nullptr)},
        {"event", p.has_event() ? protocol::decode_notice_event(p.event()) : Json(nullptr)},
        {"related_event", candidate.has_event() ? notice_object(candidate.event()) : Json(nullptr)},
        {"published", published}
    };
}

ip::NoticeSetRequest notice_set_request(const Json& value)
{
    require_fields(value, {"id", "name", "query", "program_artifact", "data_owner"});
    ip::NoticeSetRequest result;
    result.set_id(value.at("id").get<std::string>());
    result.set_name(value.at("name").get<std::string>());
    result.set_program_artifact(value.at("program_artifact").get<std::string>());
    *result.mutable_data_owner() = notice_owner(value.at("data_owner"));
    const auto& query = value.at("query");
    require_fields(query, {"events", "use_mode", "as_of_ms", "use_scope"});
    if (!query.at("events").is_array() || query.at("events").size() > 16)
        throw std::invalid_argument("event set selection exceeds its bounds");
    for (const auto& event : query.at("events"))
        *result.mutable_query()->add_events() = notice_object(event);
    const auto mode = query.at("use_mode").get<std::string>();
    if (mode != "strict_history" && mode != "post_hoc_research")
        throw std::invalid_argument("invalid notice use mode");
    result.mutable_query()->set_use_mode(
        mode == "strict_history" ? ip::STRICT_HISTORY : ip::POST_HOC_RESEARCH);
    result.mutable_query()->set_as_of_ms(notice_time(query.at("as_of_ms")));
    result.mutable_query()->set_use_scope(query.at("use_scope").get<std::string>());
    return result;
}

Json notice_set_view(const ip::NoticeSetPayload& payload)
{
    const auto& request = payload.request();
    Json events = Json::array(), members = Json::array(), exclusions = Json::array();
    for (const auto& event : request.query().events())
        events.push_back(notice_object(event));
    const auto reasons = [](const auto& values) {
        Json result = Json::array();
        for (const auto& value : values)
            result.push_back(
                {{"source", notice_object(value.source())}, {"reason", value.reason()}});
        return result;
    };
    for (const auto& member : payload.members())
        members.push_back(
            {{"event", protocol::decode_notice_event_view(member.event())},
                {"publication", notice_object(member.publication().reference())},
                {"content", notice_object(member.publication().publication().storage().content())},
                {"limitations", reasons(member.limitations())}});
    for (const auto& excluded : payload.exclusions())
        exclusions.push_back(
            {{"event", notice_object(excluded.event())}, {"reasons", reasons(excluded.reasons())}});
    return {
        {"request",
            {{"id", request.id()},
                {"name", request.name()},
                {"program_artifact", request.program_artifact()},
                {"data_owner",
                    {{"node", request.data_owner().node_id()},
                        {"service", request.data_owner().service_id()},
                        {"domain", request.data_owner().domain()}}},
                {"query",
                    {{"events", events},
                        {"as_of_ms", std::to_string(request.query().as_of_ms())},
                        {"use_scope", request.query().use_scope()},
                        {"use_mode",
                            request.query().use_mode() == ip::STRICT_HISTORY
                                ? "strict_history"
                                : "post_hoc_research"}}}}},
        {"members", members},
        {"exclusions", exclusions}
    };
}

std::uint64_t notice_position(const Json& value)
{
    const auto text = value.get<std::string>();
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size()
        || result > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument(
            "notice position must be a bounded nonnegative decimal integer");
    return result;
}

const char* notice_delivery_kind(ip::NoticeDeliveryKind kind)
{
    if (kind == ip::STATEMENT_PUBLISHED)
        return "publication";
    if (kind == ip::EVENT_CORRECTED)
        return "correction";
    if (kind == ip::EVENT_WITHDRAWN)
        return "withdrawal";
    throw std::invalid_argument("notice delivery change kind is invalid");
}

Json notice_filter(const ip::NoticeDeliveryFilter& value)
{
    Json kinds = Json::array();
    for (int kind : value.kinds())
        kinds.push_back(notice_delivery_kind(static_cast<ip::NoticeDeliveryKind>(kind)));
    return {
        {"contracts", value.contracts()},
        {"rule_id", value.rule_id()},
        {"use_scope", value.use_scope()},
        {"kinds", kinds},
        {"include_followup_statements", value.include_followup_statements()}
    };
}

ip::NoticeSubscriptionDefinition notice_subscription_definition(const Json& value)
{
    require_fields(value, {"id", "name", "filter", "start_after", "consumer"});
    ip::NoticeSubscriptionDefinition result;
    result.set_id(value.at("id").get<std::string>());
    result.set_name(value.at("name").get<std::string>());
    result.set_start_after(notice_position(value.at("start_after")));
    if (value.at("consumer") != "application_reminder")
        throw std::invalid_argument("notice subscription consumer is not implemented");
    result.set_consumer(ip::NoticeSubscriptionDefinition::APPLICATION_REMINDER);
    const auto& filter = value.at("filter");
    require_fields(
        filter,
        {"contracts", "rule_id", "use_scope", "kinds", "include_followup_statements"});
    if (!filter.at("contracts").is_array() || filter.at("contracts").size() > 16
        || !filter.at("kinds").is_array() || filter.at("kinds").size() > 3)
        throw std::invalid_argument("notice subscription conditions exceed their bounds");
    auto& selected = *result.mutable_filter();
    for (const auto& contract : filter.at("contracts"))
        selected.add_contracts(contract.get<std::string>());
    selected.set_rule_id(filter.at("rule_id").get<std::string>());
    selected.set_use_scope(filter.at("use_scope").get<std::string>());
    selected.set_include_followup_statements(filter.at("include_followup_statements").get<bool>());
    for (const auto& kind : filter.at("kinds")) {
        const auto name = kind.get<std::string>();
        if (name == "publication")
            selected.add_kinds(ip::STATEMENT_PUBLISHED);
        else if (name == "correction")
            selected.add_kinds(ip::EVENT_CORRECTED);
        else if (name == "withdrawal")
            selected.add_kinds(ip::EVENT_WITHDRAWN);
        else
            throw std::invalid_argument("notice delivery change kind is invalid");
    }
    protocol::validate_notice_subscription_definition(result);
    return result;
}

Json notice_delivery(const ip::NoticeDelivery& value)
{
    return {
        {"position", std::to_string(value.position())},
        {"source", notice_object(value.source())},
        {"event", notice_object(value.event())},
        {"initial_publication", notice_object(value.initial_publication())},
        {"contract", value.contract()},
        {"rule_id", value.rule_id()},
        {"use_scope", value.use_scope()},
        {"kind", notice_delivery_kind(value.kind())},
        {"available_at_ms", std::to_string(value.available_at_ms())}
    };
}

Json notice_delivery_page(const ip::NoticeDeliveryPage& value)
{
    Json items = Json::array();
    for (const auto& item : value.items())
        items.push_back(notice_delivery(item));
    return {
        {"owner",
            {{"node", value.owner().node_id()},
                {"service", value.owner().service_id()},
                {"domain", value.owner().domain()}}},
        {"filter", notice_filter(value.query().filter())},
        {"after", std::to_string(value.query().after())},
        {"through", std::to_string(value.through())},
        {"head", std::to_string(value.head())},
        {"scanned_through", std::to_string(value.scanned_through())},
        {"more", value.more()},
        {"items", items}
    };
}

Json notice_subscription_view(const ip::NoticeSubscriptionView& view)
{
    const auto& value = view.subscription();
    const auto& definition = value.definition();
    return {
        {"reference", notice_object(value.reference())},
        {"definition",
            {{"id", definition.id()},
                {"name", definition.name()},
                {"filter", notice_filter(definition.filter())},
                {"start_after", std::to_string(definition.start_after())},
                {"consumer", "application_reminder"}}},
        {"active", value.active()},
        {"revision", std::to_string(value.revision())},
        {"confirmed_through", std::to_string(value.confirmed_through())},
        {"paused_through", std::to_string(value.paused_through())},
        {"gap_through", std::to_string(value.gap_through())},
        {"head", std::to_string(view.head())},
        {"created_at_ms", std::to_string(value.created_at_ms())},
        {"updated_at_ms", std::to_string(value.updated_at_ms())},
        {"pending_delivery",
            value.has_pending_delivery() ? notice_object(value.pending_delivery()) : Json(nullptr)}
    };
}

Json notice_delivery_batch(const ip::NoticeDeliveryBatch& value)
{
    const auto& request = value.payload().request();
    return {
        {"reference", notice_object(value.reference())},
        {"subscription", notice_object(request.subscription())},
        {"operation_id", request.operation_id()},
        {"after", std::to_string(request.after())},
        {"through", std::to_string(request.through())},
        {"historical", request.historical()},
        {"acknowledged", value.acknowledged()},
        {"offered_at_ms", std::to_string(value.payload().offered_at_ms())},
        {"page", notice_delivery_page(value.payload().page())}
    };
}

Json notice_reminders(const ip::NoticeReminderPage& value)
{
    Json items = Json::array();
    for (const auto& item : value.items())
        items.push_back(
            {{"change", notice_delivery(item.change())},
                {"first_offered_at_ms", std::to_string(item.first_offered_at_ms())},
                {"historical", item.historical()}});
    return {
        {"subscription", notice_object(value.subscription())},
        {"after", std::to_string(value.after())},
        {"next_after", std::to_string(value.next_after())},
        {"items", items}
    };
}

} // namespace

void Application::Impl::register_intelligence_commands()
{
    for (const auto* method : {"node.intelligence.local.open", "node.intelligence.attach"})
        command(
            method,
            [this, method = std::string(method)](const json& p) -> PolledTask<Response> {
                const auto generation = intelligence_generation;
                ServiceEndpoint address;
                if (method == "node.intelligence.local.open") {
                    fields(p, {});
                    const auto node = co_await local_node_client();
                    address = co_await PollFuture{node->local_intelligence()};
                } else {
                    fields(p, {"id", "service"});
                    address = co_await PollFuture{manager.node(text(p, "id"))
                            ->service_endpoint(text(p, "service"), node::proto::INTELLIGENCE)};
                }
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                for (;;) {
                    ip::IntelligenceRequest q;
                    q.mutable_heartbeat();
                    const auto reply = co_await intelligence_call(service_io, address, q);
                    if (reply.health().initialized())
                        break;
                    if (std::chrono::steady_clock::now() >= deadline)
                        throw Error(
                            ErrorCode::unavailable,
                            "intelligence initialization timed out");
                    const auto next =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
                    co_await PollUntil{[&] {
                        return std::chrono::steady_clock::now() >= next;
                    }};
                }
                if (generation != intelligence_generation)
                    throw Error(ErrorCode::conflict, "intelligence owner selection changed");
                intelligence_endpoint = address;
                ++intelligence_generation;
                intelligence_state = {
                    {"node_id", address.node_id},
                    {"owner_node_id", address.owner_node_id},
                    {"service", address.session},
                    {"connection_id", unique_process_id()},
                    {"remote", address.endpoint.empty()},
                    {"host", address.host},
                    {"port", address.port},
                    {"records", Json::array()}
                };
                auto result = snapshot();
                result.values["intelligence_service"] = intelligence_state;
                co_return result;
            });
    for (const auto* method :
        {"intelligence.subscription.list",
            "intelligence.subscription.preview",
            "intelligence.subscription.create",
            "intelligence.subscription.get",
            "intelligence.subscription.control",
            "intelligence.subscription.offer",
            "intelligence.subscription.batch",
            "intelligence.subscription.ack",
            "intelligence.subscription.reminders"})
        command(
            method,
            [this, method = std::string(method)](const json& p) -> PolledTask<Response> {
                if (!intelligence_endpoint)
                    throw Error(ErrorCode::unavailable, "select an Intelligence service first");
                const auto address = *intelligence_endpoint;
                const auto generation = intelligence_generation;
                ip::IntelligenceRequest request;
                auto& operation = *request.mutable_subscription();
                if (method == "intelligence.subscription.list") {
                    fields(p, {});
                    operation.mutable_list();
                } else if (method == "intelligence.subscription.preview"
                    || method == "intelligence.subscription.create") {
                    fields(p, {"definition"});
                    const auto definition = notice_subscription_definition(p.at("definition"));
                    if (method == "intelligence.subscription.preview")
                        *operation.mutable_preview() = definition;
                    else
                        *operation.mutable_create() = definition;
                } else if (method == "intelligence.subscription.get") {
                    fields(p, {"id"});
                    operation.mutable_get()->set_id(text(p, "id"));
                } else if (method == "intelligence.subscription.control") {
                    fields(p, {"subscription", "revision", "active", "gap_through"});
                    auto& value = *operation.mutable_control();
                    *value.mutable_subscription() = notice_object(p.at("subscription"));
                    value.set_expected_revision(notice_position(p.at("revision")));
                    value.set_active(p.at("active").get<bool>());
                    if (!p.at("gap_through").is_null())
                        value.set_gap_through(notice_position(p.at("gap_through")));
                } else if (method == "intelligence.subscription.offer") {
                    fields(p, {"request"});
                    const auto& v = p.at("request");
                    require_fields(
                        v,
                        {"subscription", "operation_id", "after", "through", "historical"});
                    auto& value = *operation.mutable_offer();
                    *value.mutable_subscription() = notice_object(v.at("subscription"));
                    value.set_operation_id(v.at("operation_id").get<std::string>());
                    value.set_after(notice_position(v.at("after")));
                    value.set_through(notice_position(v.at("through")));
                    value.set_historical(v.at("historical").get<bool>());
                } else if (method == "intelligence.subscription.reminders") {
                    fields(p, {"subscription", "after"});
                    auto& value = *operation.mutable_reminders();
                    *value.mutable_subscription() = notice_object(p.at("subscription"));
                    value.set_after(notice_position(p.at("after")));
                    value.set_limit(64);
                } else {
                    if (method == "intelligence.subscription.ack") {
                        fields(p, {"reference", "subscription"});
                        *operation.mutable_acknowledge() = notice_object(p.at("reference"));
                    } else {
                        fields(p, {"subscription", "operation_id"});
                        *operation.mutable_delivery()->mutable_subscription() =
                            notice_object(p.at("subscription"));
                        operation.mutable_delivery()->set_operation_id(text(p, "operation_id"));
                    }
                }
                const auto reply = co_await intelligence_call(service_io, address, request);
                if (method == "intelligence.subscription.ack"
                    && reply.subscription().subscription().reference().SerializeAsString()
                        != notice_object(p.at("subscription")).SerializeAsString())
                    throw Error(
                        ErrorCode::conflict,
                        "notice delivery confirmation belongs to another subscription");
                const std::string key = reply.has_subscriptions() ? "notice_subscriptions"
                    : reply.has_deliveries()                      ? "notice_delivery_page"
                    : reply.has_delivery_batch()                  ? "notice_delivery_batch"
                    : reply.has_reminders()                       ? "notice_reminders"
                                                                  : "notice_subscription";
                auto view = co_await service_io.read<Json>([&] {
                    if (reply.has_subscriptions()) {
                        Json items = Json::array();
                        const auto& page = reply.subscriptions();
                        for (const auto& record : page.items()) {
                            ip::NoticeSubscriptionView item;
                            *item.mutable_subscription() = record;
                            item.set_head(page.head());
                            items.push_back(notice_subscription_view(item));
                        }
                        return Json{
                            {"owner",
                                {{"node", page.owner().node_id()},
                                    {"service", page.owner().service_id()},
                                    {"domain", page.owner().domain()}}},
                            {"head", std::to_string(page.head())},
                            {"items", std::move(items)}
                        };
                    }
                    if (reply.has_deliveries())
                        return notice_delivery_page(reply.deliveries());
                    if (reply.has_delivery_batch())
                        return notice_delivery_batch(reply.delivery_batch());
                    if (reply.has_reminders())
                        return notice_reminders(reply.reminders());
                    return notice_subscription_view(reply.subscription());
                });
                if (generation != intelligence_generation)
                    throw Error(ErrorCode::conflict, "intelligence owner selection changed");
                auto result = snapshot();
                result.values[key] = std::move(view);
                co_return result;
            });
    command("intelligence.source.inspect", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"source"});
        if (!intelligence_endpoint)
            throw Error(ErrorCode::unavailable, "select an Intelligence service first");
        const auto address = *intelligence_endpoint;
        const auto generation = intelligence_generation;
        const auto source = notice_object(p.at("source"));
        ip::IntelligenceRequest request;
        if (source.kind() == "intelligence_publication")
            *request.mutable_publication() = source;
        else if (source.kind() == "intelligence_change")
            request.mutable_change_get()->set_id(source.id());
        else
            throw std::invalid_argument("notice reminder source kind is not implemented");
        auto reply = co_await intelligence_call(service_io, address, request);
        if (source.kind() == "intelligence_publication") {
            request.Clear();
            *request.mutable_get()->mutable_id() =
                reply.publication().publication().candidate().id();
            reply = co_await intelligence_call(service_io, address, request);
            if (!reply.record().has_published()
                || reply.record().published().reference().SerializeAsString()
                    != source.SerializeAsString())
                throw Error(
                    ErrorCode::conflict,
                    "notice reminder publication evidence differs from its source");
        } else if (!reply.change().has_published()
            || reply.change().published().reference().SerializeAsString()
                != source.SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "notice reminder change evidence differs from its source");
        auto view = co_await service_io.read<Json>([&] {
            return reply.has_record() ? notice_record(reply.record())
                                      : protocol::decode_notice_change(reply.change());
        });
        if (generation != intelligence_generation)
            throw Error(ErrorCode::conflict, "intelligence owner selection changed");
        auto result = snapshot();
        result.values[reply.has_record() ? "intelligence_record" : "notice_change"] =
            std::move(view);
        co_return result;
    });
    for (const auto* method :
        {"intelligence.set.preview", "intelligence.set.publish", "intelligence.set.get"})
        command(
            method,
            [this, method = std::string(method)](const json& p) -> PolledTask<Response> {
                if (!intelligence_endpoint)
                    throw Error(ErrorCode::unavailable, "select an Intelligence service first");
                const auto address = *intelligence_endpoint;
                const auto generation = intelligence_generation;
                ip::IntelligenceRequest request;
                if (method == "intelligence.set.get") {
                    fields(p, {"id"});
                    request.mutable_event_set()->mutable_get()->set_id(text(p, "id"));
                } else {
                    fields(p, {"request"});
                    const auto selected = notice_set_request(p.at("request"));
                    if (method == "intelligence.set.preview")
                        *request.mutable_event_set()->mutable_preview() = selected;
                    else
                        *request.mutable_event_set()->mutable_publish() = selected;
                }
                const auto reply = co_await intelligence_call(service_io, address, request);
                auto view = co_await service_io.read<Json>([&] {
                    if (reply.has_set_preview()) {
                        auto value = notice_set_view(reply.set_preview().payload());
                        value["state"] = "preview";
                        value["observed_at_ms"] =
                            std::to_string(reply.set_preview().observed_at_ms());
                        value["reference"] = nullptr;
                        value["available_at_ms"] = nullptr;
                        return value;
                    }
                    const auto& record = reply.event_set();
                    auto value = notice_set_view(record.intent().payload());
                    value["state"] = record.has_published() ? "published" : "prepared";
                    value["observed_at_ms"] = std::to_string(record.intent().prepared_at_ms());
                    value["reference"] = record.has_published()
                        ? notice_object(record.published().reference())
                        : Json(nullptr);
                    value["available_at_ms"] = record.has_published()
                        ? Json(std::to_string(record.published().available_at_ms()))
                        : Json(nullptr);
                    return value;
                });
                if (generation != intelligence_generation)
                    throw Error(ErrorCode::conflict, "intelligence owner selection changed");
                auto result = snapshot();
                result.values["notice_set"] = std::move(view);
                co_return result;
            });
    for (const auto* method : {"intelligence.event.view", "intelligence.change"})
        command(
            method,
            [this, method = std::string(method)](const json& p) -> PolledTask<Response> {
                if (!intelligence_endpoint)
                    throw Error(ErrorCode::unavailable, "select an Intelligence service first");
                const auto address = *intelligence_endpoint;
                const auto generation = intelligence_generation;
                ip::IntelligenceRequest request;
                if (method == "intelligence.change") {
                    fields(p, {"request"});
                    *request.mutable_change() = notice_change_request(p.at("request"));
                } else {
                    fields(p, {"event", "as_of_ms"});
                    *request.mutable_event_view()->mutable_event() = notice_object(p.at("event"));
                    if (!p.at("as_of_ms").is_null())
                        request.mutable_event_view()->set_as_of_ms(notice_time(p.at("as_of_ms")));
                }
                const auto reply = co_await intelligence_call(service_io, address, request);
                auto view = co_await service_io.read<Json>([&] {
                    if (reply.has_change())
                        return protocol::decode_notice_change(reply.change());
                    return protocol::decode_notice_event_view(reply.event_view());
                });
                if (generation != intelligence_generation)
                    throw Error(ErrorCode::conflict, "intelligence owner selection changed");
                auto result = snapshot();
                result.values[reply.has_change() ? "notice_change" : "notice_event_view"] =
                    std::move(view);
                co_return result;
            });
    command("intelligence.consumers", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"source", "after"});
        if (!intelligence_endpoint)
            throw Error(ErrorCode::unavailable, "select an Intelligence service first");
        const auto address = *intelligence_endpoint;
        const auto generation = intelligence_generation;
        ip::IntelligenceRequest query;
        auto& page = *query.mutable_consumer()->mutable_list();
        *page.mutable_source() = notice_object(p.at("source"));
        page.set_after(text(p, "after", true));
        page.set_limit(64);
        const auto reply = co_await intelligence_call(service_io, address, query);
        auto view = co_await service_io.read<Json>([&] {
            Json items = Json::array();
            for (const auto& record : reply.consumer_page().items())
                items.push_back(protocol::decode_notice_consumer_record(record));
            return Json{
                {"source", notice_object(page.source())},
                {"items", std::move(items)},
                {"next_after", reply.consumer_page().next_after()}
            };
        });
        if (generation != intelligence_generation)
            throw Error(ErrorCode::conflict, "intelligence owner selection changed");
        auto result = snapshot();
        result.values["notice_consumers"] = std::move(view);
        co_return result;
    });
    for (const auto* method :
        {"intelligence.submit",
            "intelligence.get",
            "intelligence.list",
            "intelligence.review",
            "intelligence.publish",
            "intelligence.abandon"})
        command(
            method,
            [this, method = std::string(method)](const json& p) -> PolledTask<Response> {
                if (!intelligence_endpoint)
                    throw Error(ErrorCode::unavailable, "select an Intelligence service first");
                const auto address = *intelligence_endpoint;
                const auto generation = intelligence_generation;
                ip::IntelligenceRequest q;
                std::string candidate_id;
                if (method == "intelligence.submit") {
                    fields(p, {"candidate"});
                    *q.mutable_submit() = notice_candidate(p.at("candidate"));
                } else if (method == "intelligence.get") {
                    fields(p, {"id"});
                    q.mutable_get()->set_id(text(p, "id"));
                } else if (method == "intelligence.list") {
                    fields(p, {"after"});
                    q.mutable_list()->set_after(text(p, "after", true));
                    q.mutable_list()->set_limit(32);
                } else if (method == "intelligence.review") {
                    fields(p, {"id", "candidate", "operator_name", "reason", "approve", "checks"});
                    auto& r = *q.mutable_review();
                    r.set_id(text(p, "id"));
                    *r.mutable_candidate() = notice_object(p.at("candidate"));
                    r.set_operator_name(text(p, "operator_name"));
                    r.set_reason(text(p, "reason"));
                    r.set_action(
                        p.at("approve").get<bool>() ? ip::NoticeReview::APPROVE
                                                    : ip::NoticeReview::REJECT);
                    for (const auto& field : p.at("checks")) {
                        auto& c = *r.add_checks();
                        c.set_field(field.get<std::string>());
                        c.set_state(ip::PASS);
                    }
                    protocol::validate_notice_review(r);
                    candidate_id = r.candidate().id();
                } else if (method == "intelligence.publish") {
                    fields(p, {"id", "candidate", "data_owner", "reason"});
                    auto& r = *q.mutable_publish();
                    r.set_id(text(p, "id"));
                    *r.mutable_candidate() = notice_object(p.at("candidate"));
                    *r.mutable_data_owner() = notice_owner(p.at("data_owner"));
                    r.set_reason(text(p, "reason"));
                    candidate_id = r.candidate().id();
                } else {
                    fields(p, {"id", "candidate", "reason"});
                    auto& r = *q.mutable_abandon();
                    r.set_id(text(p, "id"));
                    *r.mutable_candidate() = notice_object(p.at("candidate"));
                    r.set_reason(text(p, "reason"));
                    candidate_id = r.candidate().id();
                }
                auto reply = co_await intelligence_call(service_io, address, q);
                if (!candidate_id.empty()) {
                    q.Clear();
                    q.mutable_get()->set_id(candidate_id);
                    reply = co_await intelligence_call(service_io, address, q);
                }
                auto view = co_await service_io.read<Json>([&] {
                    if (reply.has_record())
                        return notice_record(reply.record());
                    Json list = Json::array();
                    for (const auto& r : reply.records().items())
                        list.push_back(notice_record(r));
                    return list;
                });
                if (generation != intelligence_generation)
                    throw Error(ErrorCode::conflict, "intelligence owner selection changed");
                auto& records = intelligence_state.at("records");
                if (view.is_array())
                    records = view;
                else {
                    auto found = std::find_if(records.begin(), records.end(), [&](const auto& r) {
                        return r.at("reference").at("id") == view.at("reference").at("id");
                    });
                    if (found != records.end())
                        *found = view;
                    else {
                        if (records.size() == 32)
                            records.erase(records.begin());
                        records.push_back(view);
                    }
                }
                const bool listing = view.is_array();
                auto result = snapshot();
                result.values["intelligence_service"] = intelligence_state;
                result.values[listing ? "intelligence_records" : "intelligence_record"] =
                    std::move(view);
                co_return result;
            });
}

} // namespace asterion::terminal
