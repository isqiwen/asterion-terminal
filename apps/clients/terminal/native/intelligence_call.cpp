#include "intelligence_call.hpp"
#include <asterion/protocol/intelligence.hpp>
#include <asterion/protocol/resource.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/kernel/trace.hpp>

namespace asterion::terminal {

using namespace std::chrono_literals;

PolledTask<intelligence::proto::IntelligenceResponse> intelligence_call(
    ServiceIo& io,
    const ServiceEndpoint& endpoint,
    intelligence::proto::IntelligenceRequest request)
{
    if (endpoint.owner_node_id.empty())
        throw Error(ErrorCode::conflict, "intelligence endpoint has no stable owner");
    request.set_version(1);
    request.set_service_id(endpoint.session);
    request.set_correlation_id(next_correlation_id());
    auto transport = co_await io.admin<std::unique_ptr<ipc::RpcClient>>([&] {
        auto budget = io.payload_budget(ServiceIo::PayloadLane::control);
        return endpoint.endpoint.empty() ? std::make_unique<ipc::RpcClient>(
                                               io.reactor(),
                                               endpoint.host,
                                               endpoint.port,
                                               endpoint.tls,
                                               1,
                                               budget,
                                               2 * 1024 * 1024)
                                         : std::make_unique<ipc::RpcClient>(
                                               io.reactor(),
                                               endpoint.endpoint,
                                               1,
                                               budget,
                                               2 * 1024 * 1024);
    });
    auto pending =
        transport->request(request.SerializeAsString(), request.has_heartbeat() ? 3s : 15s);
    co_await PollUntil{[&] {
        transport->poll();
        return pending.wait_for(0ms) == std::future_status::ready;
    }};
    auto frame = pending.get();
    co_return co_await io.read<intelligence::proto::IntelligenceResponse>([&, frame] {
        intelligence::proto::IntelligenceResponse response;
        if (!response.ParseFromString(*frame))
            throw std::invalid_argument("invalid intelligence response");
        protocol::validate_message(response);
        if (response.version() != request.version() || response.service_id() != request.service_id()
            || response.correlation_id() != request.correlation_id())
            throw Error(ErrorCode::conflict, "intelligence response identity mismatch");
        if (response.has_error())
            throw_remote_error(response.error().code(), response.error().message());
        const auto owned = [&](const resource::proto::Object& ref) {
            if (ref.owner().node_id() != endpoint.owner_node_id
                || ref.owner().service_id() != endpoint.session
                || ref.owner().domain() != "intelligence")
                throw Error(ErrorCode::conflict, "intelligence response owner mismatch");
        };
        if (request.has_heartbeat()) {
            if (!response.has_health() || response.health().instance_id() != endpoint.session)
                throw Error(ErrorCode::conflict, "intelligence health response mismatch");
        } else if (request.has_consumer()) {
            if (!request.consumer().has_list() || !response.has_consumer_page())
                throw Error(ErrorCode::conflict, "Terminal notice custody query is read only");
            const auto& query = request.consumer().list();
            const auto limit = query.limit() ? query.limit() : 64U;
            const auto& page = response.consumer_page();
            if (limit > 128 || page.items_size() > static_cast<int>(limit))
                throw Error(ErrorCode::conflict, "notice custody page exceeds requested bounds");
            auto previous = query.after();
            for (const auto& record : page.items()) {
                protocol::validate_notice_consumer_record(record);
                owned(record.binding().source());
                if (record.binding().source().SerializeAsString()
                    != query.source().SerializeAsString())
                    throw Error(ErrorCode::conflict, "notice custody page source differs");
                const auto key = protocol::notice_consumer_key(record.binding().consumer());
                if (key <= previous)
                    throw Error(ErrorCode::conflict, "notice custody page order differs");
                previous = key;
            }
            if (!page.next_after().empty()
                && (page.items_size() != static_cast<int>(limit) || page.next_after() != previous))
                throw Error(ErrorCode::conflict, "notice custody continuation differs");
        } else if (request.has_resources()) {
            if (!response.has_resources())
                throw Error(ErrorCode::conflict, "intelligence resource response missing");
            resource::proto::Owner owner;
            owner.set_node_id(endpoint.owner_node_id);
            owner.set_service_id(endpoint.session);
            owner.set_domain("intelligence");
            (void)protocol::decode_resource_catalogue(
                response.resources(),
                request.resources(),
                owner);
        } else if (request.has_list()) {
            if (!response.has_records()
                || static_cast<unsigned>(response.records().items_size()) > request.list().limit())
                throw Error(ErrorCode::conflict, "intelligence list response mismatch");
            for (const auto& record : response.records().items()) {
                protocol::validate_notice_record(record);
                owned(record.reference());
            }
        } else if (request.has_get() || request.has_submit()) {
            if (!response.has_record())
                throw Error(ErrorCode::conflict, "intelligence candidate response missing");
            protocol::validate_notice_record(response.record());
            owned(response.record().reference());
            const auto id = request.has_get() ? request.get().id() : request.submit().id();
            if (response.record().reference().id() != id
                || (request.has_submit()
                    && response.record().input().candidate().SerializeAsString()
                        != request.submit().SerializeAsString()))
                throw Error(ErrorCode::conflict, "intelligence candidate response mismatch");
        } else if (request.has_review()) {
            if (!response.has_verification()
                || response.verification().review().SerializeAsString()
                    != request.review().SerializeAsString())
                throw Error(ErrorCode::conflict, "intelligence review response mismatch");
            owned(response.verification().review().candidate());
        } else if (request.has_publish()) {
            if (!response.has_publication())
                throw Error(ErrorCode::conflict, "intelligence publication response missing");
            protocol::validate_notice_published(response.publication());
            owned(response.publication().reference());
            if (response.publication().publication().id() != request.publish().id()
                || response.publication().publication().candidate().SerializeAsString()
                    != request.publish().candidate().SerializeAsString()
                || response.publication()
                        .publication()
                        .storage()
                        .content()
                        .owner()
                        .SerializeAsString()
                    != request.publish().data_owner().SerializeAsString())
                throw Error(ErrorCode::conflict, "intelligence publication response mismatch");
        } else if (request.has_event_set()) {
            const auto& command = request.event_set();
            if (command.has_preview()) {
                if (!response.has_set_preview())
                    throw Error(ErrorCode::conflict, "event set preview response is missing");
                protocol::validate_notice_set_payload(response.set_preview().payload());
                const auto& payload = response.set_preview().payload();
                owned(payload.request().query().events(0));
                auto expected = command.preview();
                if (expected.program_artifact().empty())
                    expected.set_program_artifact(payload.request().program_artifact());
                if (expected.SerializeAsString() != payload.request().SerializeAsString()
                    || response.set_preview().observed_at_ms() < expected.query().as_of_ms())
                    throw Error(ErrorCode::conflict, "event set preview differs from its request");
            } else {
                if (!response.has_event_set() || (!command.has_get() && !command.has_publish()))
                    throw Error(ErrorCode::conflict, "event set response is missing");
                const auto& record = response.event_set();
                protocol::validate_notice_set_record(record);
                owned(record.intent().reference());
                if ((command.has_get() && record.intent().reference().id() != command.get().id())
                    || (command.has_publish()
                        && (!record.has_published()
                            || record.intent().payload().request().SerializeAsString()
                                != command.publish().SerializeAsString())))
                    throw Error(ErrorCode::conflict, "event set response differs from its request");
            }
        } else if (request.has_subscription()) {
            const auto& command = request.subscription();
            if (command.has_list()) {
                if (!response.has_subscriptions())
                    throw Error(
                        ErrorCode::conflict,
                        "notice subscription list response is missing");
                const auto& page = response.subscriptions();
                if (page.owner().node_id() != endpoint.owner_node_id
                    || page.owner().service_id() != endpoint.session
                    || page.owner().domain() != "intelligence" || page.items_size() > 64)
                    throw Error(ErrorCode::conflict, "notice subscription list owner differs");
                std::string previous;
                for (const auto& record : page.items()) {
                    intelligence::proto::NoticeSubscriptionView view;
                    *view.mutable_subscription() = record;
                    view.set_head(page.head());
                    protocol::validate_notice_subscription_view(view);
                    owned(record.reference());
                    if (record.reference().id() <= previous)
                        throw Error(ErrorCode::conflict, "notice subscription list order differs");
                    previous = record.reference().id();
                }
            } else if (command.has_preview()) {
                if (!response.has_deliveries())
                    throw Error(
                        ErrorCode::conflict,
                        "notice subscription preview response is missing");
                protocol::validate_notice_delivery_page(response.deliveries());
                const auto& page = response.deliveries();
                if (page.owner().node_id() != endpoint.owner_node_id
                    || page.owner().service_id() != endpoint.session
                    || page.query().filter().SerializeAsString()
                        != command.preview().filter().SerializeAsString()
                    || page.query().after() != command.preview().start_after())
                    throw Error(
                        ErrorCode::conflict,
                        "notice subscription preview differs from its conditions");
            } else if (command.has_offer() || command.has_delivery()) {
                if (!response.has_delivery_batch())
                    throw Error(ErrorCode::conflict, "notice delivery batch response is missing");
                protocol::validate_notice_delivery_batch(response.delivery_batch());
                const auto& value = response.delivery_batch();
                owned(value.reference());
                if ((command.has_offer()
                        && value.payload().request().SerializeAsString()
                            != command.offer().SerializeAsString())
                    || (command.has_delivery()
                        && (value.payload().request().subscription().SerializeAsString()
                                != command.delivery().subscription().SerializeAsString()
                            || value.reference().id() != command.delivery().operation_id())))
                    throw Error(
                        ErrorCode::conflict,
                        "notice delivery batch response differs from its request");
            } else if (command.has_reminders()) {
                if (!response.has_reminders())
                    throw Error(ErrorCode::conflict, "notice reminder page response is missing");
                const auto& page = response.reminders();
                const auto& query = command.reminders();
                owned(page.subscription());
                const auto limit = query.limit() ? query.limit() : 64U;
                if (page.subscription().SerializeAsString()
                        != query.subscription().SerializeAsString()
                    || page.after() != query.after() || page.items_size() > static_cast<int>(limit))
                    throw Error(
                        ErrorCode::conflict,
                        "notice reminder page differs from its subscription");
                auto previous = query.after();
                for (const auto& item : page.items()) {
                    protocol::validate_notice_reminder(item);
                    if (item.subscription().SerializeAsString()
                            != query.subscription().SerializeAsString()
                        || item.change().position() <= previous)
                        throw Error(ErrorCode::conflict, "notice reminder page order differs");
                    previous = item.change().position();
                }
                if (page.next_after()
                    && (page.items_size() != static_cast<int>(limit)
                        || page.next_after() != previous))
                    throw Error(ErrorCode::conflict, "notice reminder continuation differs");
            } else {
                if (!response.has_subscription())
                    throw Error(ErrorCode::conflict, "notice subscription response is missing");
                protocol::validate_notice_subscription_view(response.subscription());
                const auto& value = response.subscription().subscription();
                owned(value.reference());
                if ((command.has_get() && value.reference().id() != command.get().id())
                    || (command.has_create()
                        && value.definition().SerializeAsString()
                            != command.create().SerializeAsString())
                    || (command.has_control()
                        && value.reference().SerializeAsString()
                            != command.control().subscription().SerializeAsString()))
                    throw Error(
                        ErrorCode::conflict,
                        "notice subscription response differs from its fixed identity");
            }
        } else if (request.has_publication()) {
            if (!response.has_publication())
                throw Error(ErrorCode::conflict, "notice reminder publication response is missing");
            protocol::validate_notice_published(response.publication());
            owned(response.publication().reference());
            if (response.publication().reference().SerializeAsString()
                != request.publication().SerializeAsString())
                throw Error(
                    ErrorCode::conflict,
                    "notice reminder publication response identity differs");
        } else if (request.has_change_get()) {
            if (!response.has_change())
                throw Error(ErrorCode::conflict, "notice reminder change response is missing");
            protocol::validate_notice_change_record(response.change());
            owned(response.change().intent().reference());
            if (response.change().intent().reference().id() != request.change_get().id())
                throw Error(
                    ErrorCode::conflict,
                    "notice reminder change response identity differs");
        } else if (request.has_change()) {
            if (!response.has_change())
                throw Error(ErrorCode::conflict, "notice change response is missing");
            protocol::validate_notice_change_record(response.change());
            owned(response.change().intent().reference());
            if (!response.change().has_published()
                || response.change().intent().payload().input().request().SerializeAsString()
                    != request.change().SerializeAsString())
                throw Error(ErrorCode::conflict, "notice change response differs from its request");
        } else if (request.has_event_view()) {
            if (!response.has_event_view())
                throw Error(ErrorCode::conflict, "notice event response is missing");
            const auto& view = response.event_view();
            protocol::validate_notice_event_view(view);
            owned(view.event().reference());
            if (view.event().reference().SerializeAsString()
                    != request.event_view().event().SerializeAsString()
                || (request.event_view().has_as_of_ms()
                    && (view.event().available_at_ms() > request.event_view().as_of_ms()
                        || view.has_pending_change()
                        || (view.has_latest_change()
                            && view.latest_change().published().available_at_ms()
                                > request.event_view().as_of_ms()))))
                throw Error(ErrorCode::conflict, "notice event change or cutoff differs");
        } else if (request.has_abandon()) {
            if (!response.has_release_decision()
                || response.release_decision().object().SerializeAsString()
                    != request.abandon().candidate().SerializeAsString()
                || response.release_decision().state()
                    != resource::proto::ReleaseDecision::RELEASED)
                throw Error(ErrorCode::conflict, "intelligence abandonment response mismatch");
            owned(response.release_decision().object());
        } else
            throw std::invalid_argument("unsupported Terminal intelligence request");
        return response;
    });
}

} // namespace asterion::terminal
