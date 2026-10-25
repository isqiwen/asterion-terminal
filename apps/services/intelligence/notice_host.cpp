#include "notice_host.hpp"
#include <algorithm>
#include <asterion/protocol/intelligence.hpp>
#include <asterion/protocol/retention_release.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/data_client.hpp>
#include <asterion/protocol/service_discovery.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/trace.hpp>

namespace asterion::intelligence {

using namespace std::chrono_literals;

NoticeHost::NoticeHost(Configuration configuration)
    : configuration_(std::move(configuration)),
      process_(unique_process_id())
{
    if (configuration_.discovery_endpoint.empty())
        throw std::invalid_argument("intelligence requires the owning Agent discovery endpoint");
}

void NoticeHost::open()
{
    opening_ = writer_.submit([this](std::stop_token) {
        store_ = std::make_unique<Store>(configuration_.directory, configuration_.owner);
        programs::Files files(configuration_.directory / "programs", configuration_.owner);
        programme_ = files.preserve(current_executable(), configuration_.program_artifact);
        store_->set_programme(programme_);
    });
}

data::proto::DataResponse NoticeHost::data_call(
    const resource::proto::Owner& owner,
    data::proto::DataRequest request,
    std::stop_token stop,
    std::chrono::steady_clock::time_point deadline) const
{
    if (owner.domain() != "data" || owner.node_id() != configuration_.owner.node_id())
        throw Error(ErrorCode::unavailable, "cross-node intelligence source transfer unavailable");
    const auto budget = [&](std::chrono::milliseconds maximum) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "intelligence source request stopped");
        if (deadline == std::chrono::steady_clock::time_point::max())
            return maximum;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left <= 0ms)
            throw Error(ErrorCode::unavailable, "event set preview source deadline expired");
        return std::min(maximum, left);
    };
    const auto status =
        protocol::service_discovery(configuration_.discovery_endpoint, budget(3s), stop);
    if (status.node_id() != owner.node_id() || status.phase() != node::proto::Status::READY
        || status.has_failure())
        throw Error(ErrorCode::unavailable, "intelligence source discovery is not ready");
    const node::proto::Service* peer = nullptr;
    for (const auto& service : status.services())
        if (service.id() == owner.service_id()) {
            if (peer || service.kind() != node::proto::DATA)
                throw Error(ErrorCode::conflict, "intelligence Data owner mismatch");
            peer = &service;
        }
    if (!peer || peer->state() != "running" || peer->endpoint().empty())
        throw Error(ErrorCode::unavailable, "selected intelligence Data owner unavailable");
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "intelligence source request stopped");
    return protocol::DataClient(peer->endpoint(), owner.service_id())
        .call(std::move(request), budget(8s));
}

PolledTask<proto::NoticeRecord> NoticeHost::submit(proto::NoticeCandidate candidate)
{
    protocol::validate_notice_candidate(candidate);
    std::optional<proto::NoticeRecord> existing;
    co_await write([&] {
        try {
            existing = store_->get(candidate.id());
        } catch (const Error& e) {
            if (e.code() != ErrorCode::not_found)
                throw;
        }
    });
    proto::NoticeRecord record;
    if (existing) {
        if (existing->input().candidate().SerializeAsString() != candidate.SerializeAsString())
            throw Error(ErrorCode::conflict, "notice candidate id already has different input");
        record = *existing;
        if (record.state() != proto::PREPARING)
            co_return record;
    } else {
        data::proto::MaterialRead material;
        std::optional<data::proto::TableListedContracts> dictionary;
        co_await read([&](std::stop_token stop) {
            data::proto::DataRequest request;
            request.mutable_material_read()->set_id(candidate.material().id());
            *request.mutable_material_read()->mutable_expected_reference() = candidate.material();
            material = data_call(candidate.material().owner(), request, stop).material_read();
            if (!candidate.dictionary().snapshot().empty()) {
                request.Clear();
                *request.mutable_table_listed_contracts() = candidate.dictionary();
                dictionary =
                    data_call(candidate.material().owner(), request, stop).table_listed_contracts();
                if (dictionary->query().SerializeAsString()
                    != candidate.dictionary().SerializeAsString())
                    throw Error(
                        ErrorCode::conflict,
                        "notice dictionary response differs from fixed query");
            }
        });
        auto prepared =
            prepare_notice(configuration_.owner, candidate, material, dictionary, programme_);
        co_await write([&] {
            record = store_->stage(prepared);
        });
    }
    std::optional<data::proto::DataRetentionRecord> confirmed;
    if (record.input().has_dictionary_reference()) {
        data::proto::DataRetention retention;
        const auto& source = record.input().dictionary_reference().owner();
        retention.mutable_source()->set_node_id(source.node_id());
        retention.mutable_source()->set_service_id(source.service_id());
        *retention.mutable_consumer() = record.reference();
        auto& version = *retention.add_versions();
        version.set_kind(data::proto::DataVersion::TABLE_SNAPSHOT);
        version.set_version(record.input().dictionary_reference().version());
        co_await read([&](std::stop_token stop) {
            data::proto::DataRequest request;
            *request.mutable_retention()->mutable_prepare() = retention;
            data_call(source, request, stop);
            request.Clear();
            *request.mutable_retention()->mutable_confirm() = retention;
            confirmed = data_call(source, request, stop).retention();
        });
    }
    co_await write([&] {
        record = store_->admit(record.reference(), confirmed);
    });
    co_return record;
}

PolledTask<proto::NoticeChangeRecord> NoticeHost::change(proto::NoticeChangeRequest request)
{
    protocol::validate_notice_change_request(request);
    std::optional<proto::NoticeChangeRecord> prior;
    co_await write([&] {
        try {
            prior = store_->change(request.id());
            if (prior->intent().payload().input().request().SerializeAsString()
                != request.SerializeAsString())
                throw Error(ErrorCode::conflict, "notice change operation has another request");
        } catch (const Error& error) {
            if (error.code() != ErrorCode::not_found)
                throw;
        }
    });
    if (prior && prior->has_published())
        co_return *prior;
    proto::NoticeChangeIntent intent;
    if (prior)
        intent = prior->intent();
    else {
        data::proto::MaterialRead material;
        co_await read([&](std::stop_token stop) {
            data::proto::DataRequest query;
            query.mutable_material_read()->set_id(request.material().id());
            *query.mutable_material_read()->mutable_expected_reference() = request.material();
            material =
                data_call(request.material().owner(), std::move(query), stop).material_read();
        });
        auto prepared = prepare_notice_change(configuration_.owner, request, material, programme_);
        co_await write([&] {
            intent = store_->begin_change(prepared);
        });
    }
    data::proto::DataResponse saved;
    co_await read([&](std::stop_token stop) {
        data::proto::DataRequest query;
        *query.mutable_intelligence_change_save() = intent.reference();
        saved = data_call(intent.data_owner(), std::move(query), stop);
    });
    proto::NoticeChangeRecord result;
    co_await write([&] {
        result = store_->confirm_change(intent.reference(), saved.intelligence_saved());
    });
    co_return result;
}

PolledTask<std::string> NoticeHost::serve(std::string frame, bool health)
{
    proto::IntelligenceRequest request;
    proto::IntelligenceResponse response;
    response.set_version(1);
    response.set_service_id(configuration_.owner.service_id());
    try {
        if (frame.size() > 1024 * 1024 || !request.ParseFromString(frame))
            throw std::invalid_argument("invalid intelligence request");
        protocol::validate_message(request);
        response.set_correlation_id(request.correlation_id());
        if (request.version() != 1 || request.service_id() != configuration_.owner.service_id()
            || request.correlation_id().empty())
            throw Error(ErrorCode::conflict, "intelligence request identity mismatch");
        if (health && !request.has_heartbeat() && !request.has_quiesce())
            throw Error(ErrorCode::permission_denied, "intelligence health lane only");
        if (request.has_quiesce()) {
            if (!health)
                throw Error(
                    ErrorCode::permission_denied,
                    "intelligence upgrade control requires health lane");
            quiescing_ = request.quiesce().stop();
        }
        if (request.has_heartbeat() || request.has_quiesce()) {
            auto& h = *response.mutable_health();
            h.set_instance_id(configuration_.owner.service_id());
            h.set_process_id(process_);
            h.set_initialized(initialized_);
            h.set_quiescing(quiescing_);
        } else {
            if (!initialized_)
                throw Error(ErrorCode::unavailable, "intelligence is initializing");
            const bool mutation = request.has_submit() || request.has_review()
                || (request.has_event_set() && request.event_set().has_publish())
                || (request.has_subscription()
                    && (request.subscription().has_create() || request.subscription().has_control()
                        || request.subscription().has_offer()
                        || request.subscription().has_acknowledge()))
                || request.has_publish() || request.has_change() || request.has_abandon()
                || (request.has_consumer()
                    && (request.consumer().has_prepare() || request.consumer().has_confirm()
                        || request.consumer().has_release()));
            if (quiescing_ && mutation)
                throw Error(ErrorCode::unavailable, "intelligence is preparing for upgrade");
            if (request.has_subscription()) {
                const auto& command = request.subscription();
                co_await write([&] {
                    if (command.has_list())
                        *response.mutable_subscriptions() = store_->subscriptions();
                    else if (command.has_preview())
                        *response.mutable_deliveries() =
                            store_->preview_subscription(command.preview());
                    else if (command.has_create())
                        *response.mutable_subscription() =
                            store_->create_subscription(command.create());
                    else if (command.has_get())
                        *response.mutable_subscription() = store_->subscription(command.get().id());
                    else if (command.has_control())
                        *response.mutable_subscription() =
                            store_->control_subscription(command.control());
                    else if (command.has_offer())
                        *response.mutable_delivery_batch() =
                            store_->offer_delivery(command.offer());
                    else if (command.has_acknowledge())
                        *response.mutable_subscription() =
                            store_->acknowledge_delivery(command.acknowledge());
                    else if (command.has_delivery())
                        *response.mutable_delivery_batch() =
                            store_->delivery_batch(command.delivery());
                    else if (command.has_reminders())
                        *response.mutable_reminders() = store_->reminders(command.reminders());
                    else
                        throw std::invalid_argument("notice subscription operation is required");
                });
            } else if (request.has_consumer()) {
                const auto& command = request.consumer();
                if (command.has_release()) {
                    proto::NoticeConsumerRecord record;
                    co_await write([&] {
                        record = store_->consumer(command.release());
                    });
                    if (record.phase() == proto::NoticeConsumerRecord::RELEASED) {
                        *response.mutable_consumer() = record;
                    } else {
                        resource::proto::ReleaseDecision proof;
                        co_await read([&](std::stop_token stop) {
                            proof = protocol::verify_retention_release(
                                command.release(),
                                configuration_.owner.node_id(),
                                configuration_.discovery_endpoint,
                                stop);
                        });
                        co_await write([&] {
                            *response.mutable_consumer() =
                                store_->release_consumer(command.release(), proof);
                        });
                    }
                } else {
                    co_await write([&] {
                        if (command.has_prepare())
                            *response.mutable_consumer() =
                                store_->prepare_consumer(command.prepare());
                        else if (command.has_confirm())
                            *response.mutable_consumer() =
                                store_->confirm_consumer(command.confirm());
                        else if (command.has_get())
                            *response.mutable_consumer() = store_->consumer(command.get());
                        else if (command.has_list())
                            *response.mutable_consumer_page() = store_->consumers(command.list());
                        else
                            throw std::invalid_argument("notice consumer operation is required");
                    });
                }
            } else if (request.has_event_set()) {
                const auto& command = request.event_set();
                if (command.has_preview()) {
                    proto::NoticeSetPreview preview;
                    co_await write([&] {
                        preview = store_->preview_set(command.preview());
                    });
                    co_await read([&](std::stop_token stop) {
                        const auto deadline = std::chrono::steady_clock::now() + 10s;
                        for (const auto& member : preview.payload().members()) {
                            data::proto::DataRequest query;
                            *query.mutable_intelligence_read() =
                                member.publication().publication().preparation();
                            const auto saved = data_call(
                                preview.payload().request().data_owner(),
                                std::move(query),
                                stop,
                                deadline)
                                                   .intelligence_content();
                            if (saved.receipt().SerializeAsString()
                                != member.publication().publication().storage().SerializeAsString())
                                throw Error(
                                    ErrorCode::conflict,
                                    "event set preview statement storage differs");
                            if (member.event().has_latest_change()) {
                                const auto& change = member.event().latest_change();
                                query.Clear();
                                *query.mutable_intelligence_change_read() =
                                    change.intent().reference();
                                const auto evidence = data_call(
                                    preview.payload().request().data_owner(),
                                    std::move(query),
                                    stop,
                                    deadline)
                                                          .intelligence_change_content();
                                if (evidence.receipt().SerializeAsString()
                                    != change.published().storage().SerializeAsString())
                                    throw Error(
                                        ErrorCode::conflict,
                                        "event set preview change storage differs");
                            }
                        }
                    });
                    *response.mutable_set_preview() = std::move(preview);
                } else if (command.has_publish()) {
                    proto::NoticeSetIntent intent;
                    std::optional<proto::NoticeSetRecord> published;
                    co_await write([&] {
                        intent = store_->begin_set(command.publish());
                        const auto record = store_->event_set(command.publish().id());
                        if (record.has_published())
                            published = record;
                    });
                    if (published)
                        *response.mutable_event_set() = *published;
                    else {
                        data::proto::DataResponse saved;
                        co_await read([&](std::stop_token stop) {
                            data::proto::DataRequest query;
                            *query.mutable_intelligence_set_save() = intent.reference();
                            saved = data_call(intent.data_owner(), std::move(query), stop);
                        });
                        co_await write([&] {
                            *response.mutable_event_set() =
                                store_->confirm_set(intent.reference(), saved.intelligence_saved());
                        });
                    }
                } else {
                    co_await write([&] {
                        if (command.has_get())
                            *response.mutable_event_set() = store_->event_set(command.get().id());
                        else if (command.has_preparation())
                            *response.mutable_set_preparation() =
                                store_->set_preparation(command.preparation());
                        else if (command.has_publication())
                            *response.mutable_set_publication() =
                                store_->set_publication(command.publication());
                        else
                            throw std::invalid_argument("event set operation is required");
                    });
                }
            } else if (request.has_submit()) {
                *response.mutable_record() = co_await submit(request.submit());
            } else if (request.has_change()) {
                *response.mutable_change() = co_await change(request.change());
            } else if (request.has_publish()) {
                proto::NoticePublicationIntent intent;
                std::optional<proto::NoticePublished> published;
                co_await write([&] {
                    intent = store_->begin_publication(request.publish());
                    const auto current = store_->get(request.publish().candidate().id());
                    if (current.state() == proto::PUBLISHED)
                        published = current.published();
                });
                if (published) {
                    *response.mutable_publication() = *published;
                    co_return response.SerializeAsString();
                }
                data::proto::DataResponse saved;
                co_await read([&](std::stop_token stop) {
                    data::proto::DataRequest q;
                    *q.mutable_intelligence_save() = intent.reference();
                    saved = data_call(intent.data_owner(), std::move(q), stop);
                });
                co_await write([&] {
                    *response.mutable_publication() = store_->confirm_publication(
                        request.publish().candidate(),
                        saved.intelligence_saved());
                });
            } else {
                co_await write([&] {
                    if (request.has_deliveries())
                        *response.mutable_deliveries() = store_->deliveries(request.deliveries());
                    else if (request.has_resources())
                        *response.mutable_resources() = store_->resources(request.resources());
                    else if (request.has_get())
                        *response.mutable_record() = store_->get(request.get().id());
                    else if (request.has_list()) {
                        const auto& q = request.list();
                        auto& result = *response.mutable_records();
                        for (const auto& item : store_->list(
                                 q.after(),
                                 q.limit(),
                                 q.has_state() ? std::optional<proto::NoticeState>(q.state())
                                               : std::nullopt))
                            *result.add_items() = item;
                    } else if (request.has_review())
                        *response.mutable_verification() = store_->review(request.review());
                    else if (request.has_preparation())
                        *response.mutable_preparation() =
                            store_->preparation(request.preparation());
                    else if (request.has_publication())
                        *response.mutable_publication() =
                            store_->publication(request.publication());
                    else if (request.has_change_preparation())
                        *response.mutable_change_preparation() =
                            store_->change_preparation(request.change_preparation());
                    else if (request.has_change_get())
                        *response.mutable_change() = store_->change(request.change_get().id());
                    else if (request.has_event_view())
                        *response.mutable_event_view() = store_->event_view(request.event_view());
                    else if (request.has_event())
                        *response.mutable_event() = store_->event(request.event());
                    else if (request.has_abandon()) {
                        const auto& q = request.abandon();
                        *response.mutable_release_decision() =
                            store_->abandon(q.candidate(), q.id(), q.reason());
                    } else if (request.has_release_decision())
                        *response.mutable_release_decision() =
                            store_->release_decision(request.release_decision());
                    else
                        throw std::invalid_argument("intelligence operation is required");
                });
            }
        }
    } catch (const std::exception& e) {
        response.clear_result();
        response.mutable_error()->set_code(std::string(error_name(classify(e))));
        response.mutable_error()->set_message(e.what());
    }
    co_return response.SerializeAsString();
}

service::RpcHost::Reply NoticeHost::accept(std::string frame, bool health)
{
    auto task = std::make_shared<PolledTask<std::string>>(serve(std::move(frame), health));
    return [task]() -> std::optional<service::RpcHost::Message> {
        if (!task->poll())
            return {};
        return task->take();
    };
}

bool NoticeHost::advance(service::RpcHost::Stage stage)
{
    if (opening_.valid() && opening_.wait_for(0s) == std::future_status::ready) {
        opening_.get();
        initialized_ = true;
    }
    if (stage != service::RpcHost::Stage::running)
        quiescing_ = true;
    if (stage == service::RpcHost::Stage::stopping_resources) {
        readers_.shutdown();
        writer_.shutdown();
        return true;
    }
    return false;
}

} // namespace asterion::intelligence
