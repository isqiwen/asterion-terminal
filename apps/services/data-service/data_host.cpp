#include "data_host.hpp"
#include <asterion/protocol/retention_release.hpp>
#include "observation_source.hpp"
#include "intelligence_source.hpp"
#include "material_providers.hpp"
#include "historical_margins.hpp"
#include "history_coverage.hpp"
#include "history_minutes.hpp"
#include "history_providers.hpp"
#include "history_update.hpp"
#include "table_series.hpp"
#include "table_update.hpp"
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <algorithm>
#include <fstream>

namespace asterion::data {

using namespace std::chrono_literals;
namespace wire = proto;

namespace {

void fail(wire::DataResponse& response, const std::exception& error)
{
    response.mutable_error()->set_code(std::string(error_name(classify(error))));
    response.mutable_error()->set_message(error.what());
}

std::int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

DataHost::DataHost(Configuration configuration)
    : configuration_(std::move(configuration)),
      process_(unique_process_id()),
      readers_(configuration_.file_workers, 8, service::wake_io_owner)
{}

void DataHost::open()
{
    opening_ = writer_.submit([this](std::stop_token) {
        const auto directory = std::filesystem::absolute(configuration_.directory);
        store_ = std::make_unique<Store>(directory, configuration_.service);
        if (!configuration_.node_id.empty()) {
            wire::DataOwner owner;
            owner.set_node_id(configuration_.node_id);
            owner.set_service_id(configuration_.service);
            retentions_ = std::make_unique<Retentions>(directory, std::move(owner), *store_);
            resource::proto::Owner archive_owner;
            archive_owner.set_node_id(configuration_.node_id);
            archive_owner.set_service_id(configuration_.service);
            archive_owner.set_domain("data");
            materials_ = std::make_unique<Materials>(directory, archive_owner);
            intelligence_content_ = std::make_unique<IntelligenceContent>(directory, archive_owner);
            observations_ = std::make_unique<Observations>(directory, std::move(archive_owner));
        }
        named_dataset_count_ = store_->archive().named_datasets().items_size();
        budgets_ = std::make_unique<DownloadBudget>(directory / "budgets");
        if (!configuration_.internal_endpoint.empty()) {
            std::filesystem::create_directories(directory / "acquisition-tasks");
            refresh_ = std::make_unique<AcquisitionTasks>(
                directory / "acquisition-tasks",
                configuration_.service,
                configuration_.internal_endpoint,
                *store_,
                configuration_.node_id,
                materials_.get());
        }
    });
}

wire::DataResponse DataHost::health_response(const wire::DataRequest& request) const
{
    wire::DataResponse response;
    response.set_version(1);
    response.set_service_id(configuration_.service);
    response.set_correlation_id(request.correlation_id());
    auto* health = response.mutable_health();
    health->set_instance_id(configuration_.service);
    health->set_process_id(process_);
    health->set_version(ASTERION_PRODUCT_VERSION);
    health->set_initialized(initialized_);
    health->set_recovery_required(degraded_);
    health->set_uptime_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_)
            .count());
    return response;
}

wire::DataRequest DataHost::parse(const std::string& frame) const
{
    wire::DataRequest request;
    if (!request.ParseFromString(frame))
        throw std::invalid_argument("invalid data Protobuf");
    protocol::validate_message(request);
    validate_id(request.correlation_id());
    if (request.version() != 1 || request.service_id() != configuration_.service)
        throw std::invalid_argument("data protocol version or service mismatch");
    return request;
}

void DataHost::require_ready() const
{
    if (degraded_)
        throw Error(ErrorCode::unavailable, "data persistence failed; restart data service");
}

bool DataHost::is_turn(const Work& work) const
{
    for (const auto& key : work.objects)
        if (object_order_.at(key).front() != &work)
            return false;
    return true;
}

void DataHost::release(Work& work)
{
    for (const auto& key : work.objects) {
        auto& queue = object_order_.at(key);
        std::erase(queue, &work);
        if (queue.empty())
            object_order_.erase(key);
    }
    work.objects.clear();
    if (std::exchange(work.reading_catalog, false))
        --catalog_readers_;
    if (std::exchange(work.awaiting_catalog_write, false))
        --catalog_waiting_writers_;
    if (std::exchange(work.writing_catalog, false))
        catalog_writing_ = false;
}

std::function<void()> DataHost::prepare(
    const wire::DataRequest& request,
    wire::DataResponse& response,
    std::stop_token stop,
    bool& creates_dataset,
    std::string& publication_dataset)
{
    if (request.has_resources()) {
        if (configuration_.node_id.empty())
            throw Error(
                ErrorCode::unavailable,
                "resource catalogue requires stable Agent node identity");
        resource::proto::Owner owner;
        owner.set_node_id(configuration_.node_id);
        owner.set_service_id(configuration_.service);
        owner.set_domain("data");
        *response.mutable_resources() = store_->resources(owner, request.resources(), stop);
        return {};
    } else if (request.has_table_retirements()) {
        auto* page = response.mutable_table_retirements();
        auto entries = store_->tables().retirements();
        std::ranges::sort(entries, {}, &history_files::TableStore::Retirement::snapshot);
        for (const auto& item : entries) {
            if (item.snapshot <= request.table_retirements().id())
                continue;
            if (page->items_size() == 256) {
                page->set_next_after(page->items(255).id());
                break;
            }
            auto* row = page->add_items();
            row->set_id(item.snapshot);
            row->set_sequence(item.sequence);
            row->set_state(
                item.complete ? wire::TableRetirement::REMOVED : wire::TableRetirement::PENDING);
        }
        return {};
    } else if (request.has_table_retirement()) {
        if (!retentions_)
            throw Error(ErrorCode::unavailable, "table retirement requires Agent node identity");
        const auto operation = request.table_retirement();
        const auto prior = store_->tables().retirement(operation.id());
        const bool head = store_->tables().head() == operation.id();
        std::optional<history_files::TableStore::Retirement> decision = prior;
        if (!decision && !head)
            decision = store_->tables().preview_retirement(operation.id(), stop);
        auto* result = response.mutable_table_retirement();
        result->set_id(operation.id());
        result->set_current_head(head);
        if (decision) {
            const auto digest = sha256_bytes(
                Json::array({configuration_.node_id,
                                configuration_.service,
                                operation.id(),
                                decision->sequence,
                                decision->objects})
                    .dump());
            result->set_preview_digest(digest);
            if (operation.execute() && !decision->complete && operation.preview_digest() != digest)
                throw Error(
                    ErrorCode::conflict,
                    "table retirement scope changed; preview and confirm again");
            result->set_sequence(decision->sequence);
            result->set_payload_bytes(store_->tables().retirement_bytes(*decision));
            for (const auto& id : decision->objects)
                result->add_objects(id);
        } else {
            const auto sequence = store_->tables().sequence(operation.id());
            if (!sequence)
                throw Error(ErrorCode::not_found, "unknown table snapshot");
            result->set_sequence(*sequence);
            result->set_payload_bytes(0);
        }
        return [&, operation, prior, head, decision] {
            try {
                auto* result = response.mutable_table_retirement();
                if (prior) {
                    result->set_state(
                        prior->complete ? wire::TableRetirement::REMOVED
                                        : wire::TableRetirement::PENDING);
                    return;
                }
                if (!refresh_)
                    throw Error(ErrorCode::unavailable, "acquisition authority unavailable");
                *result->mutable_named_usage() = store_->snapshot_usage(operation.id());
                for (const auto& id : refresh_->snapshot_holds(operation.id()))
                    result->add_acquisitions(id);
                wire::DataVersion version;
                version.set_kind(wire::DataVersion::TABLE_SNAPSHOT);
                version.set_version(operation.id());
                for (const auto& hold : retentions_->holds(version))
                    *result->add_holds() = hold;
                const bool blocked = head || result->named_usage().references_size()
                    || result->acquisitions_size() || result->holds_size();
                result->set_state(
                    blocked ? wire::TableRetirement::BLOCKED : wire::TableRetirement::AVAILABLE);
                if (operation.execute() && !blocked) {
                    store_->begin_table_retirement(*decision);
                    result->set_state(wire::TableRetirement::PENDING);
                }
            } catch (const Error& error) {
                fail(response, error);
            } catch (const std::invalid_argument& error) {
                fail(response, error);
            } catch (const std::out_of_range&) {
                fail(
                    response,
                    Error(ErrorCode::unavailable, "snapshot observation owner record is missing"));
            }
        };
    } else if (request.has_minute_retirements()) {
        const auto& query = request.minute_retirements();
        const auto limit = query.limit() ? query.limit() : 64U;
        if (limit > 256)
            throw std::invalid_argument("minute retirement page exceeds capacity");
        const auto entries = store_->archive().retirements(query.after(), limit + 1);
        auto* result = response.mutable_minute_retirements();
        for (std::size_t i = 0; i < std::min<std::size_t>(entries.size(), limit); ++i) {
            auto* item = result->add_items();
            item->set_id(entries[i].record().minute_result().manifest_sha256());
            item->set_state(
                entries[i].complete() ? wire::MinuteRetirement::REMOVED
                                      : wire::MinuteRetirement::PENDING);
        }
        if (entries.size() > limit)
            result->set_next_after(result->items(result->items_size() - 1).id());
        return {};
    } else if (request.has_minute_retirement()) {
        if (!retentions_)
            throw Error(
                ErrorCode::unavailable,
                "minute retirement requires its Agent node identity");
        const auto& operation = request.minute_retirement();
        const auto prior = store_->archive().retirement(operation.id());
        wire::HistoryRecord record;
        std::string manifest;
        if (prior)
            response.mutable_minute_retirement()->set_payload_bytes(
                store_->archive().retirement_payload_bytes(prior->record(), stop));
        if (!prior) {
            record = store_->archive().get(operation.id());
            if (!std::filesystem::is_directory(record.minute_result().directory()))
                throw Error(ErrorCode::unavailable, "minute payload is unavailable");
            response.mutable_minute_retirement()->set_payload_bytes(
                store_->archive().retirement_payload_bytes(record, stop));
            *response.mutable_minute_retirement()->mutable_internal_usage() =
                store_->history_usage(operation.id());
            if (operation.execute()) {
                const auto path =
                    std::filesystem::path(record.minute_result().directory()) / "minutes.json";
                if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path)
                    || std::filesystem::file_size(path) > 8 * 1024 * 1024)
                    throw std::invalid_argument("invalid minute retirement manifest");
                std::ifstream input(path, std::ios::binary);
                manifest.assign(std::istreambuf_iterator<char>(input), {});
                if (input.bad())
                    throw std::runtime_error("minute retirement manifest read failed");
            }
        }
        return [&, prior, record = std::move(record), manifest = std::move(manifest), operation] {
            try {
                auto* result = response.mutable_minute_retirement();
                result->set_id(operation.id());
                if (prior) {
                    result->set_state(
                        prior->complete() ? wire::MinuteRetirement::REMOVED
                                          : wire::MinuteRetirement::PENDING);
                    return;
                }
                if (!refresh_)
                    throw Error(
                        ErrorCode::unavailable,
                        "minute acquisition authority is unavailable");
                wire::AcquisitionTaskRequest usage;
                usage.mutable_active_history_usage()->set_id(operation.id());
                const auto owned = refresh_->handle(usage).history_usage();
                for (const auto& reference : owned.references())
                    *result->mutable_internal_usage()->add_references() = reference;
                wire::DataVersion version;
                version.set_kind(wire::DataVersion::MINUTE_VERSION);
                version.set_version(operation.id());
                for (const auto& hold : retentions_->holds(version))
                    *result->add_holds() = hold;
                const bool blocked =
                    result->internal_usage().references_size() || result->holds_size();
                result->set_state(
                    blocked ? wire::MinuteRetirement::BLOCKED : wire::MinuteRetirement::AVAILABLE);
                if (operation.execute() && !blocked) {
                    store_->begin_minute_retirement(record, manifest);
                    result->set_state(wire::MinuteRetirement::PENDING);
                }
            } catch (const std::invalid_argument& error) {
                fail(response, error);
            } catch (const Error& error) {
                fail(response, error);
            }
        };
    } else if (request.has_retention()) {
        if (!retentions_)
            throw Error(ErrorCode::unavailable, "data retention requires its Agent node identity");
        std::optional<resource::proto::ReleaseDecision> release;
        if (request.retention().has_release())
            release = protocol::verify_retention_release(
                request.retention().release(),
                configuration_.node_id,
                configuration_.discovery_endpoint,
                stop);
        return [&, operation = request.retention(), release = std::move(release)] {
            try {
                auto* result = response.mutable_retention();
                if (operation.has_prepare())
                    *result = retentions_->prepare(operation.prepare());
                else if (operation.has_confirm())
                    *result = retentions_->confirm(operation.confirm());
                else if (operation.has_get())
                    *result = retentions_->get(operation.get());
                else if (operation.has_release())
                    *result = retentions_->release(operation.release(), *release);
                else
                    throw std::invalid_argument("data retention operation is required");
            } catch (const std::invalid_argument& error) {
                fail(response, error);
            } catch (const Error& error) {
                if (error.code() != ErrorCode::invalid_request
                    && error.code() != ErrorCode::conflict && error.code() != ErrorCode::not_found
                    && error.code() != ErrorCode::resource_exhausted)
                    throw;
                fail(response, error);
            }
        };
    } else if (request.has_intelligence_set_save()) {
        if (!intelligence_content_)
            throw Error(ErrorCode::unavailable, "event set content requires stable Data owner");
        const auto source = request.intelligence_set_save();
        if (const auto previous = intelligence_content_->find_set(source)) {
            *response.mutable_intelligence_saved() = previous->receipt;
            return {};
        }
        auto intent = fetch_intelligence_set_preparation(
            source,
            configuration_.node_id,
            configuration_.discovery_endpoint,
            stop);
        return [&, source, intent = std::move(intent)] {
            *response.mutable_intelligence_saved() =
                intelligence_content_->save_set(source, [&](const auto&) {
                    return intent;
                });
        };
    } else if (request.has_intelligence_set_read()) {
        if (!intelligence_content_)
            throw Error(ErrorCode::unavailable, "event set content requires stable Data owner");
        const auto read = intelligence_content_->read_set(request.intelligence_set_read());
        auto& result = *response.mutable_intelligence_set_content();
        *result.mutable_receipt() = read.receipt;
        *result.mutable_payload() = read.payload;
    } else if (request.has_intelligence_change_save()) {
        if (!intelligence_content_ || !materials_)
            throw Error(ErrorCode::unavailable, "notice change content requires stable Data owner");
        const auto source = request.intelligence_change_save();
        if (const auto previous = intelligence_content_->find_change(source)) {
            *response.mutable_intelligence_saved() = previous->receipt;
            return {};
        }
        auto intent = fetch_intelligence_change_preparation(
            source,
            configuration_.node_id,
            configuration_.discovery_endpoint,
            stop);
        protocol::validate_notice_change_intent(intent);
        const auto& original = intent.payload().input().material();
        const auto evidence = materials_->read(original.reference().id(), original.reference());
        if (evidence.receipt.SerializeAsString() != original.SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "notice change material receipt differs from Data authority");
        return [&, source, intent = std::move(intent)] {
            *response.mutable_intelligence_saved() =
                intelligence_content_->save_change(source, [&](const auto&) {
                    return intent;
                });
        };
    } else if (request.has_intelligence_change_read()) {
        if (!intelligence_content_)
            throw Error(ErrorCode::unavailable, "notice change content requires stable Data owner");
        const auto read = intelligence_content_->read_change(request.intelligence_change_read());
        auto& result = *response.mutable_intelligence_change_content();
        *result.mutable_receipt() = read.receipt;
        *result.mutable_payload() = read.payload;
    } else if (request.has_intelligence_save()) {
        if (!intelligence_content_ || !retentions_)
            throw Error(ErrorCode::unavailable, "intelligence content requires stable Data owner");
        const auto source = request.intelligence_save();
        // Immutable reads happen on the reader. Never block Data's writer while
        // Intelligence serves the prepared publication from its own serial writer.
        if (const auto previous = intelligence_content_->find(source)) {
            *response.mutable_intelligence_saved() = previous->receipt;
            return {};
        }
        auto intent = fetch_intelligence_preparation(
            source,
            configuration_.node_id,
            configuration_.discovery_endpoint,
            stop);
        return [&, source, intent = std::move(intent)] {
            const auto& input = intent.payload().input();
            const auto held = retentions_->get(intent.payload().candidate());
            bool exact_snapshot = false;
            for (const auto& version : held.retention().versions())
                if (version.kind() == wire::DataVersion::TABLE_SNAPSHOT
                    && version.version() == input.dictionary_reference().version())
                    exact_snapshot = true;
            if (held.phase() != wire::DataRetentionRecord::CONFIRMED || !exact_snapshot
                || held.retention().consumer().SerializeAsString()
                    != intent.payload().candidate().SerializeAsString())
                throw Error(ErrorCode::conflict, "intelligence source retention is not confirmed");
            *response.mutable_intelligence_saved() =
                intelligence_content_->save(source, [&](const auto&) {
                    return intent;
                });
        };
    } else if (request.has_intelligence_read()) {
        if (!intelligence_content_)
            throw Error(ErrorCode::unavailable, "intelligence content requires stable Data owner");
        const auto read = intelligence_content_->read(request.intelligence_read());
        auto& result = *response.mutable_intelligence_content();
        *result.mutable_receipt() = read.receipt;
        *result.mutable_payload() = read.payload;
    } else if (request.has_material_sources()) {
        for (const auto& source : material_providers::sources()) {
            auto* item = response.mutable_material_sources()->add_items();
            item->set_id(source.id);
            item->set_name(source.name);
            item->set_usage(source.usage);
            item->set_plugin_id(source.plugin_id);
            item->set_provider_artifact(source.artifact);
            item->set_max_bytes(source.max_bytes);
            item->set_credential_required(source.credential_required);
            item->set_no_additional_fee(source.no_additional_fee);
        }
        if (!response.has_material_sources())
            response.mutable_material_sources();
    } else if (request.has_material_read()) {
        if (!materials_)
            throw Error(ErrorCode::unavailable, "material reading requires stable Data owner");
        const auto& query = request.material_read();
        auto read = materials_->read(
            query.id(),
            query.has_expected_reference()
                ? std::optional<resource::proto::Object>(query.expected_reference())
                : std::nullopt);
        auto& result = *response.mutable_material_read();
        *result.mutable_receipt() = std::move(read.receipt);
        result.set_original(std::move(read.bytes));
        *result.mutable_text() = std::move(read.text);
    } else if (request.has_acquisition_task()) {
        return [&, request] {
            if (!refresh_)
                throw Error(ErrorCode::unavailable, "data service is initializing");
            try {
                *response.mutable_acquisition_task() = refresh_->handle(request.acquisition_task());
            } catch (const std::invalid_argument& error) {
                fail(response, error);
            } catch (const std::out_of_range&) {
                fail(response, Error(ErrorCode::not_found, "unknown task"));
            } catch (const Error& error) {
                if (error.code() != ErrorCode::invalid_request
                    && error.code() != ErrorCode::conflict
                    && error.code() != ErrorCode::resource_exhausted
                    && error.code() != ErrorCode::not_found
                    && error.code() != ErrorCode::permission_denied
                    && error.code() != ErrorCode::unavailable)
                    throw;
                fail(response, error);
            }
        };
    } else if (request.has_observation()) {
        if (!observations_)
            throw Error(
                ErrorCode::unavailable,
                "observation archiving requires a stable Data owner");
        const auto& query = request.observation();
        if (query.has_admit()) {
            auto admission = query.admit();
            observations_->validate_admission(admission);
            return [&, admission = std::move(admission)] {
                *response.mutable_observation()->mutable_operation() =
                    observations_->admit(admission);
            };
        }
        if (query.has_execute()) {
            auto prepared = observations_->prepare(
                query.execute(),
                [&](const auto& source, std::stop_token token) {
                    return fetch_market_observation(
                        source,
                        configuration_.node_id,
                        configuration_.discovery_endpoint,
                        token);
                },
                stop);
            return [&, prepared = std::move(prepared)] {
                *response.mutable_observation()->mutable_operation() =
                    observations_->publish(prepared);
            };
        }
        if (!query.has_read())
            throw std::invalid_argument("observation operation is missing");
        *response.mutable_observation() = observations_->read(query.read());
    } else if (request.has_authorize_download()) {
        auto verified = store_->verify_authorization(request.authorize_download(), stop);
        return [&, verified = std::move(verified)] {
            *response.mutable_download_authorization() = store_->authorize(verified);
        };
    } else if (request.has_download_authorization()) {
        *response.mutable_download_authorization() =
            store_->authorization(request.download_authorization().id());
    } else if (request.has_download_credentials()) {
        *response.mutable_download_credentials() =
            store_->credentials(request.download_credentials());
    } else if (request.has_save_dataset()) {
        auto verified = store_->verify_dataset(request.save_dataset());
        creates_dataset = verified.creates_entry();
        return [&, verified = std::move(verified)] {
            store_->save(verified);
            *response.mutable_saved_dataset() = request.save_dataset();
        };
    } else if (request.has_prepare_download()) {
        auto verified = store_->verify_download(request.prepare_download());
        return [&, verified = std::move(verified)] {
            *response.mutable_prepared_download() = store_->prepare(verified);
        };
    } else if (request.has_allocate_download()) {
        auto verified = store_->verify_allocation(request.allocate_download());
        return [&, verified = std::move(verified)] {
            *response.mutable_download_directory() = store_->allocate(verified);
        };
    } else if (request.has_publish_download()) {
        auto verified = store_->verify_publication(request.publish_download());
        publication_dataset = verified.dataset_id();
        return [&, verified = std::move(verified)] {
            *response.mutable_published_download() = store_->publish(verified);
        };
    } else if (request.has_published_download())
        *response.mutable_published_download() = store_->published(request.published_download());
    else if (request.has_history_usage()) {
        *response.mutable_history_usage() = store_->history_usage(request.history_usage().id());
        return [&, id = request.history_usage().id()] {
            wire::AcquisitionTaskRequest query;
            query.mutable_history_usage()->set_id(id);
            const auto owned = refresh_->handle(query).history_usage();
            for (const auto& reference : owned.references())
                *response.mutable_history_usage()->add_references() = reference;
        };
    } else if (request.has_record())
        *response.mutable_record() = store_->archive().get(request.record().id());
    else if (request.has_saved_dataset())
        *response.mutable_saved_dataset() =
            store_->archive().named_dataset(request.saved_dataset().id());
    else if (request.has_saved_datasets())
        *response.mutable_saved_datasets() = store_->archive().named_datasets();
    else if (request.has_datasets()) {
        const auto& filter = request.datasets();
        auto* result = response.mutable_datasets();
        const auto listed = store_->archive().datasets(
            {filter.venue(), filter.product(), filter.contract_id(), filter.source()});
        for (const auto& item : listed) {
            auto* row = result->add_items();
            row->set_id(item.id);
            row->set_contract_id(item.contract.key());
            row->set_source(item.source);
            row->set_revision(item.revision);
            row->set_begin(item.begin);
            row->set_end(item.end);
            row->set_interval_minutes(item.interval_minutes);
            row->set_rows(item.rows);
        }
    } else if (request.has_minute_page()) {
        const auto& query = request.minute_page();
        if (query.dataset_id().empty() || !query.task_id().empty())
            throw std::invalid_argument("data query requires a published dataset identity");
        const auto record = store_->archive().get(query.dataset_id());
        *response.mutable_minute_page() =
            history_files::read_minute_page(record.minutes(), record.minute_result(), query);
    } else if (request.has_minute_range())
        *response.mutable_minute_range() =
            minute_range_evidence(store_->archive(), store_->tables(), request.minute_range());
    else if (request.has_minute_evidence())
        *response.mutable_minute_evidence() =
            minute_evidence(store_->archive(), request.minute_evidence());
    else if (request.has_historical_margins())
        *response.mutable_historical_margins() =
            historical_margins(store_->tables(), request.historical_margins());
    else if (request.has_bar_dataset())
        *response.mutable_bar_dataset() =
            resolve_bar_dataset(store_->tables(), store_->sources(request.bar_dataset()));
    else if (request.has_dominant_series()) {
        if (request.dominant_series().months_size()
            > (request.dominant_series().first_day().empty()
                    ? 20
                    : static_cast<int>(max_account_contracts)))
            throw std::invalid_argument("a dominant series requires 2 to 20 month contracts");
        // The months a request lists, or the product's own in the daily table.
        DominantSeries resolved;
        if (request.dominant_series().has_product()) {
            if (request.dominant_series().months_size())
                throw std::invalid_argument("a dominant series names its months or its product");
            resolved =
                resolve_product_series(store_->tables(), request.dominant_series().product());
        } else {
            std::vector<BarDatasetSources> months;
            for (const auto& month : request.dominant_series().months())
                months.push_back(store_->sources(month));
            resolved = resolve_dominant_series(
                store_->tables(),
                months,
                request.dominant_series().first_day());
        }
        auto* preview = response.mutable_dominant_series();
        for (const auto index : resolved.months)
            preview->add_months(static_cast<unsigned>(index));
        for (auto& dataset : resolved.datasets)
            *preview->add_datasets() = std::move(dataset);
        *preview->mutable_schedule() = std::move(resolved.schedule);
    } else if (request.has_coverage())
        *response.mutable_coverage() =
            history_coverage(store_->archive(), store_->tables(), request.coverage());
    else if (request.has_update_plan())
        *response.mutable_update_plan() = history_update_plan(
            store_->archive(),
            store_->tables(),
            request.update_plan(),
            now_ns());
    else if (request.has_verify_connection()) {
        auto* result = response.mutable_connection_verification();
        for (const auto& check : history_providers::verify_connection(
                 request.verify_connection().source(),
                 request.verify_connection().credential(),
                 stop)) {
            auto* row = result->add_checks();
            row->set_scope(check.scope);
            row->set_state(check.state);
        }
    } else if (request.has_sources()) {
        auto* result = response.mutable_sources();
        for (const auto& source : history_providers::sources()) {
            auto* out = result->add_sources();
            out->set_id(source.id);
            out->set_name(source.name);
            out->set_plugin_id(source.plugin_id);
            out->set_normalization(source.semantics.normalization);
            out->set_timezone(source.semantics.timezone);
            out->set_timestamp_semantics(source.semantics.timestamp_semantics);
            for (const auto& venue : source.venues)
                out->add_venues(venue);
            for (auto interval : source.intervals)
                out->add_intervals(interval);
            out->set_max_requests_per_minute(source.max_requests_per_minute);
            out->set_credential_required(source.credential_required);
            if (source.connection) {
                auto* schema = out->mutable_connection();
                schema->set_credential_label_en(source.connection->credential_label_en);
                schema->set_credential_label_zh(source.connection->credential_label_zh);
                schema->set_credential_required(source.connection->credential_required);
                schema->set_remember_allowed(source.connection->remember_allowed);
                schema->set_credential_max_length(source.connection->credential_max_length);
                schema->set_requests_per_minute_default(
                    source.connection->requests_per_minute_default);
                schema->set_requests_per_minute_max(source.connection->requests_per_minute_max);
            }
        }
    } else if (request.has_store_table_partition()) {
        auto verified = store_->verify_partition(request.store_table_partition());
        if (auto* receipt = std::get_if<wire::TablePartitionReceipt>(&verified))
            *response.mutable_table_partition() = std::move(*receipt);
        else
            return [&, verified = std::get<Store::VerifiedPartition>(std::move(verified))] {
                *response.mutable_table_partition() = store_->record_partition(verified, now_ns());
            };
    } else if (request.has_table_plan())
        *response.mutable_table_plan() =
            store_->table_plan(request.table_plan(), format_shanghai_time(now_ns()).substr(0, 10));
    else if (request.has_table_refresh_plan())
        *response.mutable_table_refresh_plan() = table_refresh_plan(
            store_->tables(),
            store_->table_offers(),
            request.table_refresh_plan(),
            history_providers::table_artifact(request.table_refresh_plan().provider()),
            format_shanghai_time(now_ns()).substr(0, 10));
    else if (request.has_table_coverage())
        *response.mutable_table_coverage() =
            table_coverage(store_->tables(), store_->table_offers());
    else if (request.has_table_page())
        *response.mutable_table_page() = table_page(store_->tables(), request.table_page());
    else if (request.has_table_series())
        *response.mutable_table_series() = table_series(store_->tables(), request.table_series());
    else if (request.has_table_products())
        *response.mutable_table_products() =
            table_products(store_->tables(), request.table_products());
    else if (request.has_table_listed_contracts())
        *response.mutable_table_listed_contracts() =
            table_listed_contracts(store_->tables(), request.table_listed_contracts());
    else if (request.has_table_daily())
        *response.mutable_table_daily() = table_daily(store_->tables(), request.table_daily());
    else if (request.has_table_contracts()) {
        // A history source downloads the contracts its own data source lists:
        // their codes are that provider's.
        const auto& query = request.table_contracts();
        const auto sources = history_providers::sources();
        const auto source =
            std::ranges::find(sources, query.source(), &history_providers::Source::id);
        if (source == sources.end())
            throw std::invalid_argument("unsupported historical data source");
        const auto offers = store_->table_offers();
        const auto lists = std::ranges::find_if(offers, [&](const TableOffer& offer) {
            return offer.plugin_id == source->plugin_id && offer.series.table == contracts_table;
        });
        if (lists == offers.end())
            throw Error(
                ErrorCode::conflict,
                "the data source of " + query.source() + " offers no contract list");
        *response.mutable_table_contracts() = table_contracts(
            store_->tables(),
            query.snapshot(),
            query.source(),
            lists->series.source,
            query.exchange(),
            query.product());
    } else
        throw std::invalid_argument("missing data operation");
    return {};
}

void DataHost::write(const std::shared_ptr<Work>& work)
{
    auto commit = std::exchange(work->commit, {});
    work->completion = writer_.submit([work, commit = std::move(commit)](std::stop_token) {
        TraceScope trace(work->request.correlation_id());
        commit();
        work->frame = work->response.SerializeAsString();
    });
}

std::optional<std::string> DataHost::budget_step(const std::shared_ptr<Work>& work)
{
    const bool configuring = work->request.has_configure_download_budget();
    try {
        if (!work->completion.valid()) {
            // One configuration is decided and committed at a time. Pending
            // callers retain only their bounded RPC slot; permit reads continue.
            require_ready();
            if (!is_turn(*work))
                return {};
            work->completion = readers_.submit([&, work, configuring](std::stop_token) {
                TraceScope trace(work->request.correlation_id());
                if (configuring)
                    *work->response.mutable_download_budget() =
                        store_->verify_budget(work->request.configure_download_budget());
                else
                    work->authorization =
                        store_->download_authorization(work->request.acquire_download_permit());
            });
            return {};
        }
        if (work->completion.wait_for(0ms) != std::future_status::ready)
            return {};
        work->completion.get();
        if (!work->budget_configuration)
            require_ready();
        if (configuring) {
            if (!work->budget_configuration) {
                const auto& input = work->request.configure_download_budget();
                auto configuration = budgets_->configure(
                    work->response.download_budget().provider_id(),
                    input.credential(),
                    input.requests_per_minute());
                work->completion = writer_.submit(
                    [&, configuration, trace_id = work->request.correlation_id()](std::stop_token) {
                        TraceScope trace(trace_id);
                        budgets_->persist(configuration);
                    });
                work->budget_configuration = std::move(configuration);
                return {};
            }
            budgets_->committed(*work->budget_configuration);
        } else {
            *work->response.mutable_download_permit() = budgets_->acquire(
                work->authorization.authorization().provider_id(),
                work->authorization.credential());
        }
    } catch (const std::exception& error) {
        if (work->budget_configuration) {
            budgets_->persistence_failed();
            degraded_ = true;
        }
        fail(work->response, error);
    }
    release(*work);
    protocol::log_rpc_result(
        "data-service",
        work->request,
        work->response,
        false,
        {{"service_id", configuration_.service}});
    return work->response.SerializeAsString();
}

void DataHost::admit(Work& admitted, bool worker)
{
    auto* const work = &admitted;
    const auto& request = work->request;
    if (!initialized_)
        throw Error(ErrorCode::unavailable, "data service is initializing");
    require_ready();
    const bool mutation = request.has_intelligence_set_save() || request.has_intelligence_save()
        || request.has_intelligence_change_save() || request.has_configure_download_budget()
        || request.has_acquire_download_permit() || request.has_authorize_download()
        || (request.has_observation() && !request.observation().has_read())
        || request.has_download_authorization() || request.has_download_credentials()
        || request.has_save_dataset() || request.has_allocate_download()
        || request.has_prepare_download() || request.has_publish_download()
        || request.has_store_table_partition()
        || (request.has_acquisition_task() && request.acquisition_task().has_retirement()
            && request.acquisition_task().retirement().execute())
        || (request.has_table_retirement() && request.table_retirement().execute())
        || (request.has_minute_retirement() && request.minute_retirement().execute())
        || (request.has_retention() && !request.retention().has_get());
    const bool private_operation = request.has_allocate_download() || request.has_prepare_download()
        || request.has_publish_download() || request.has_published_download()
        || request.has_download_authorization() || request.has_download_credentials()
        || request.has_acquire_download_permit() || request.has_table_plan()
        || request.has_store_table_partition();
    if (private_operation && !worker)
        throw Error(
            ErrorCode::permission_denied,
            "download coordination requires the private data channel");
    const bool finishing_refresh = worker
        && ((request.has_download_credentials()
                && request.download_credentials().task_instance() == configuration_.service)
            || (request.has_acquire_download_permit()
                && request.acquire_download_permit().task_instance() == configuration_.service)
            || (request.has_table_plan()
                && request.table_plan().identity().task_instance() == configuration_.service)
            || (request.has_store_table_partition()
                && request.store_table_partition().identity().task_instance()
                    == configuration_.service));
    const bool starts_refresh = request.has_acquisition_task()
        && (request.acquisition_task().has_submit() || request.acquisition_task().has_retry()
            || request.acquisition_task().has_material_submit());
    if (quiescing_ && ((mutation && !finishing_refresh) || starts_refresh))
        throw Error(ErrorCode::unavailable, "data service is preparing for upgrade");
    const auto attempt = [&](const wire::DownloadIdentity& identity) {
        work->objects.push_back("download/" + identity.SerializeAsString());
    };
    if (request.has_intelligence_set_save() || request.has_intelligence_set_read()) {
        const auto& source = request.has_intelligence_set_save() ? request.intelligence_set_save()
                                                                 : request.intelligence_set_read();
        protocol::validate_message(source);
        work->objects.push_back("intelligence/" + sha256_bytes(source.SerializeAsString()));
    }
    if (request.has_intelligence_change_save() || request.has_intelligence_change_read()) {
        const auto& source = request.has_intelligence_change_save()
            ? request.intelligence_change_save()
            : request.intelligence_change_read();
        protocol::validate_message(source);
        work->objects.push_back("intelligence/" + sha256_bytes(source.SerializeAsString()));
    }
    if (request.has_intelligence_save() || request.has_intelligence_read()) {
        const auto& source = request.has_intelligence_save() ? request.intelligence_save()
                                                             : request.intelligence_read();
        protocol::validate_message(source);
        work->objects.push_back("intelligence/" + sha256_bytes(source.SerializeAsString()));
    }
    if (request.has_observation()) {
        const auto& query = request.observation();
        const auto id = query.has_admit() ? query.admit().id()
            : query.has_execute()         ? query.execute()
                                          : query.read();
        validate_id(id);
        work->objects.push_back("observation/" + id);
    }
    if (request.has_authorize_download())
        work->objects.push_back(
            "authorization/"
            + sha256_bytes(
                configuration_.service + "/" + request.authorize_download().task_instance() + "/"
                + request.authorize_download().task_id()));
    else if (request.has_download_authorization())
        work->objects.push_back("authorization/" + request.download_authorization().id());
    else if (request.has_allocate_download()) {
        attempt(request.allocate_download().identity());
        work->objects.push_back("authorization/" + request.allocate_download().authorization_id());
    } else if (request.has_prepare_download())
        attempt(request.prepare_download().identity());
    else if (request.has_publish_download())
        attempt(request.publish_download().identity());
    else if (request.has_published_download())
        attempt(request.published_download());
    else if (request.has_download_credentials())
        attempt(request.download_credentials());
    else if (request.has_acquire_download_permit())
        attempt(request.acquire_download_permit());
    else if (request.has_table_plan())
        attempt(request.table_plan().identity());
    else if (request.has_store_table_partition()) {
        // One writer at a time for a partition, whichever attempt delivers it.
        const auto& partition = request.store_table_partition().partition();
        attempt(request.store_table_partition().identity());
        work->objects.push_back("table/" + partition.SerializeAsString());
    } else if (request.has_save_dataset())
        work->objects.push_back("dataset/" + request.save_dataset().id());
    else if (request.has_configure_download_budget())
        work->objects.push_back("budget-configuration");
    const auto history = [&](const std::string& id) {
        work->objects.push_back("history/" + id);
    };
    const auto selection = [&](const wire::BarDatasetRequest& input) {
        for (const auto& id : input.source_dataset_ids())
            history(id);
    };
    if (request.has_retention()) {
        const auto& operation = request.retention();
        auto consumer = operation.has_get() ? operation.get()
            : operation.has_release()       ? operation.release()
            : operation.has_prepare()       ? operation.prepare().consumer()
                                            : operation.confirm().consumer();
        consumer.clear_version();
        work->objects.push_back("retention/" + consumer.SerializeAsString());
        if (operation.has_prepare() || operation.has_confirm()) {
            const auto& retention =
                operation.has_prepare() ? operation.prepare() : operation.confirm();
            for (const auto& version : retention.versions())
                if (version.kind() == wire::DataVersion::MINUTE_VERSION)
                    history(version.version());
                else
                    work->objects.push_back("snapshot/" + version.version());
        }
    }
    if (request.has_minute_retirement())
        history(request.minute_retirement().id());
    else if (request.has_record())
        history(request.record().id());
    else if (request.has_minute_page())
        history(request.minute_page().dataset_id());
    else if (request.has_minute_range())
        history(request.minute_range().dataset_id());
    else if (request.has_minute_evidence())
        history(request.minute_evidence().dataset_id());
    else if (request.has_bar_dataset())
        selection(request.bar_dataset());
    else if (request.has_dominant_series())
        for (const auto& month : request.dominant_series().months())
            selection(month);
    else if (request.has_save_dataset())
        for (const auto& input : request.save_dataset().selections())
            selection(input);
    else if (request.has_saved_dataset())
        work->objects.push_back("dataset/" + request.saved_dataset().id());
    else if (request.has_update_plan())
        history(request.update_plan().dataset_id());
    std::ranges::sort(work->objects);
    work->objects.erase(
        std::unique(work->objects.begin(), work->objects.end()),
        work->objects.end());
    for (const auto& key : work->objects)
        object_order_[key].push_back(work);
    work->writes_catalog = request.has_intelligence_set_save() || request.has_intelligence_save()
        || request.has_intelligence_change_save() || request.has_store_table_partition()
        || request.has_save_dataset()
        || (request.has_observation() && !request.observation().has_read())
        || request.has_publish_download()
        || (request.has_acquisition_task() && request.acquisition_task().has_retirement()
            && request.acquisition_task().retirement().execute())
        || (request.has_table_retirement() && request.table_retirement().execute())
        || (request.has_minute_retirement() && request.minute_retirement().execute())
        || (request.has_retention() && !request.retention().has_get());
    // Enumeration shares the directory publication barrier. Reads of known
    // immutable versions use object identities and remain available while a
    // different version is being committed.
    work->reads_catalog = request.has_resources() || request.has_table_retirement()
        || (request.has_observation() && request.observation().has_read())
        || request.has_table_retirements() || request.has_table_coverage()
        || request.has_table_page() || request.has_table_series() || request.has_table_products()
        || request.has_table_daily() || request.has_table_contracts()
        || request.has_table_listed_contracts() || request.has_historical_margins()
        || request.has_bar_dataset() || request.has_table_refresh_plan()
        || request.has_save_dataset() || request.has_minute_retirements()
        || request.has_minute_retirement() || request.has_history_usage()
        || request.has_saved_datasets() || request.has_datasets() || request.has_coverage();
}

service::RpcHost::Reply DataHost::accept(std::string frame, bool worker)
{
    auto work = std::make_shared<Work>();
    auto& request = work->request;
    auto& response = work->response;
    try {
        request = parse(frame);
        response = health_response(request);
        if (request.has_heartbeat())
            return [frame = response.SerializeAsString()] {
                return std::optional<std::string>(frame);
            };
        response.clear_result();
        admit(*work, worker);
        if (request.has_configure_download_budget() || request.has_acquire_download_permit())
            return [this, work]() -> std::optional<std::string> {
                return budget_step(work);
            };
    } catch (const std::exception& error) {
        response.set_version(1);
        response.set_service_id(configuration_.service);
        response.set_correlation_id(request.correlation_id());
        fail(response, error);
        work->frame = response.SerializeAsString();
        work->phase = Work::Phase::complete;
    }
    return [this, work]() -> std::optional<std::string> {
        return step(work);
    };
}

std::optional<std::string> DataHost::step(const std::shared_ptr<Work>& work)
{
    try {
        if (work->phase == Work::Phase::queued) {
            require_ready();
            const bool table_write = work->request.has_store_table_partition();
            if (!is_turn(*work)
                || (work->reads_catalog && (catalog_writing_ || catalog_waiting_writers_))
                || (table_write
                    && (catalog_readers_ || catalog_writing_ || catalog_waiting_writers_)))
                return {};
            if (table_write)
                catalog_writing_ = work->writing_catalog = true;
            work->completion = readers_.submit([&, work](std::stop_token stop) {
                TraceScope trace(work->request.correlation_id());
                work->commit = prepare(
                    work->request,
                    work->response,
                    stop,
                    work->creates_dataset,
                    work->publication_dataset);
                if (!work->commit)
                    work->frame = work->response.SerializeAsString();
            });
            if (work->reads_catalog) {
                ++catalog_readers_;
                work->reading_catalog = true;
            }
            work->phase = Work::Phase::preparing;
            return {};
        }
        if (work->phase == Work::Phase::preparing) {
            if (work->completion.wait_for(0ms) != std::future_status::ready)
                return {};
            work->completion.get();
            if (std::exchange(work->reading_catalog, false))
                --catalog_readers_;
            if (work->commit) {
                if (!work->publication_dataset.empty()) {
                    auto key = "history/" + work->publication_dataset;
                    object_order_[key].push_back(work.get());
                    work->objects.push_back(std::move(key));
                }
                work->phase = Work::Phase::prepared;
                if (work->writes_catalog && !work->writing_catalog) {
                    ++catalog_waiting_writers_;
                    work->awaiting_catalog_write = true;
                }
            } else {
                require_ready();
                work->phase = Work::Phase::complete;
            }
        }
        if (work->phase == Work::Phase::prepared) {
            require_ready();
            if (!is_turn(*work)
                || (work->writes_catalog && !work->writing_catalog
                    && (catalog_readers_ || catalog_writing_)))
                return {};
            if (work->creates_dataset && named_dataset_count_ >= 1000)
                throw Error(ErrorCode::resource_exhausted, "saved dataset capacity reached");
            // Quiesce closes admission; accepted preparation may still commit.
            write(work);
            if (work->writes_catalog && !work->writing_catalog) {
                --catalog_waiting_writers_;
                work->awaiting_catalog_write = false;
                catalog_writing_ = work->writing_catalog = true;
            }
            work->phase = Work::Phase::writing;
            return {};
        }
        if (work->phase == Work::Phase::writing) {
            if (work->completion.wait_for(0ms) != std::future_status::ready)
                return {};
            work->completion.get();
            if (work->request.has_minute_retirement() && work->request.minute_retirement().execute()
                && work->response.has_minute_retirement()
                && work->response.minute_retirement().state() == wire::MinuteRetirement::PENDING) {
                work->completion = readers_.submit([&, work](std::stop_token stop) {
                    store_->clean_minute_retirement(work->request.minute_retirement().id(), stop);
                });
                work->phase = Work::Phase::cleaning;
                return {};
            }
            if (work->request.has_table_retirement() && work->request.table_retirement().execute()
                && work->response.has_table_retirement()
                && work->response.table_retirement().state() == wire::TableRetirement::PENDING) {
                work->completion = readers_.submit([&, work](std::stop_token stop) {
                    store_->clean_table_retirement(work->request.table_retirement().id(), stop);
                });
                work->phase = Work::Phase::cleaning;
                return {};
            }
            if (work->creates_dataset)
                ++named_dataset_count_;
            work->phase = Work::Phase::complete;
        }
        if (work->phase == Work::Phase::cleaning) {
            if (work->completion.wait_for(0ms) != std::future_status::ready)
                return {};
            work->completion.get();
            work->commit = [&, work] {
                if (work->request.has_table_retirement()) {
                    store_->finish_table_retirement(work->request.table_retirement().id());
                    work->response.mutable_table_retirement()->set_state(
                        wire::TableRetirement::REMOVED);
                    work->response.mutable_table_retirement()->set_payload_bytes(0);
                    return;
                }
                store_->finish_minute_retirement(work->request.minute_retirement().id());
                work->response.mutable_minute_retirement()->set_state(
                    wire::MinuteRetirement::REMOVED);
                work->response.mutable_minute_retirement()->set_payload_bytes(0);
            };
            write(work);
            work->phase = Work::Phase::writing;
            return {};
        }
    } catch (const std::exception& error) {
        if (work->phase == Work::Phase::writing || work->phase == Work::Phase::cleaning)
            degraded_ = true;
        fail(work->response, error);
        work->frame = work->response.SerializeAsString();
    }
    release(*work);
    protocol::log_rpc_result(
        "data-service",
        work->request,
        work->response,
        false,
        {{"service_id", configuration_.service}});
    return std::move(work->frame);
}

service::RpcHost::Reply DataHost::health(const std::string& frame)
{
    const auto request = parse(frame);
    if (request.has_quiesce()) {
        quiescing_ = true;
        if (request.quiesce().stop()) {
            if (!initialized_ || refreshing_.valid() || !refresh_idle_) {
                auto response = health_response(request);
                response.clear_result();
                fail(
                    response,
                    Error(ErrorCode::unavailable, "data service has running refresh tasks"));
                return [frame = response.SerializeAsString()] {
                    return std::optional<std::string>(frame);
                };
            }
            service::request_stop();
        }
    } else if (!request.has_heartbeat())
        throw std::invalid_argument("health channel only accepts heartbeat");
    return [frame = health_response(request).SerializeAsString()] {
        return std::optional<std::string>(frame);
    };
}

bool DataHost::advance(service::RpcHost::Stage stage)
{
    if (opening_.valid() && opening_.wait_for(0ms) == std::future_status::ready) {
        opening_.get();
        initialized_ = true;
    }
    if (stage != service::RpcHost::Stage::running)
        quiescing_ = true;
    // All admitted preparations and commits have completed when business
    // replies drained. The I/O owner never waits on a running file operation.
    if (refreshing_.valid() && refreshing_.wait_for(0ms) == std::future_status::ready) {
        try {
            refreshing_.get();
            catalog_writing_ = false;
        } catch (const std::exception&) {
            catalog_writing_ = false;
            degraded_ = true;
            throw;
        }
    }
    if (initialized_ && !degraded_ && !refreshing_.valid() && !catalog_readers_ && !catalog_writing_
        && !catalog_waiting_writers_
        && (stage != service::RpcHost::Stage::stopping_resources || !refresh_idle_)
        && std::chrono::steady_clock::now() >= next_refresh_) {
        const bool stopping = stage == service::RpcHost::Stage::stopping_resources;
        const bool admitting = !quiescing_;
        try {
            refreshing_ = writer_.submit([this, stopping, admitting](std::stop_token) {
                refresh_idle_ = !refresh_ || refresh_->advance(admitting, stopping);
            });
            catalog_writing_ = true;
        } catch (const Error& error) {
            // No tick was admitted: a full journal queue is backpressure, not task failure.
            if (error.code() != ErrorCode::resource_exhausted)
                throw;
        }
        next_refresh_ = std::chrono::steady_clock::now() + 100ms;
    }
    return stage == service::RpcHost::Stage::stopping_resources && !opening_.valid()
        && !refreshing_.valid() && refresh_idle_;
}

} // namespace asterion::data
