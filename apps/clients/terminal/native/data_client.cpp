#include "data_client.hpp"
#include <asterion/protocol/observation.hpp>
#include <asterion/protocol/material.hpp>
#include <asterion/protocol/resource.hpp>
#include <asterion/protocol/data_client.hpp>
#include <asterion/protocol/backtest.hpp>
#include <asterion/protocol/task.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/ipc/rpc_client.hpp>
#include <asterion/system/trace.hpp>
#include <algorithm>
#include <array>

namespace asterion::terminal {

using namespace std::chrono_literals;
namespace wire = data::proto;

namespace {

Json retirement_status(const wire::MinuteRetirement& value)
{
    const char* state = nullptr;
    switch (value.state()) {
        case wire::MinuteRetirement::AVAILABLE:
            state = "available";
            break;
        case wire::MinuteRetirement::BLOCKED:
            state = "blocked";
            break;
        case wire::MinuteRetirement::PENDING:
            state = "pending";
            break;
        case wire::MinuteRetirement::REMOVED:
            state = "removed";
            break;
        default:
            throw Error(ErrorCode::unavailable, "invalid minute retirement state");
    }
    Json result{
        {"id", value.id()},
        {"state", state},
        {"payload_bytes",
            value.has_payload_bytes() ? Json(std::to_string(value.payload_bytes()))
                                      : Json(nullptr)},
        {"references", Json::array()},
        {"consumers", Json::array()}
    };
    if (value.has_internal_usage()) {
        if (value.internal_usage().dataset_id() != value.id())
            throw Error(ErrorCode::unavailable, "minute retirement reference identity mismatch");
        result["references"] =
            protocol::decode_history_usage(value.internal_usage()).at("references");
    }
    for (const auto& hold : value.holds()) {
        const auto& consumer = hold.retention().consumer();
        if (hold.phase() != wire::DataRetentionRecord::PREPARED
            && hold.phase() != wire::DataRetentionRecord::CONFIRMED)
            throw Error(ErrorCode::unavailable, "invalid minute retirement hold");
        result["consumers"].push_back(
            {{"node", consumer.owner().node_id()},
                {"service", consumer.owner().service_id()},
                {"domain", consumer.owner().domain()},
                {"kind", consumer.kind()},
                {"id", consumer.id()},
                {"version", consumer.version()},
                {"phase",
                    hold.phase() == wire::DataRetentionRecord::PREPARED ? "prepared"
                                                                        : "confirmed"}});
    }
    return result;
}

} // namespace

struct DataClient::Impl : std::enable_shared_from_this<Impl>
{
    ServiceIo& io;
    const ServiceEndpoint endpoint;
    const std::string connection_id = unique_process_id();
    enum class Channel
    {
        request,
        observation,
        read
    };
    std::array<std::unique_ptr<ipc::RpcClient>, 3> transports;
    std::stop_source lifetime;
    Json sources = Json::array();
    std::vector<data::proto::HistorySource> typed_sources;
    Json task_page{
        {"capacity", nullptr},
        {"before_sequence", 0},
        {"next_before_sequence", 0},
        {"failed_count", 0},
        {"interrupted_count", 0},
        {"tasks", Json::array()}
    };
    unsigned before_sequence = 0;
    bool online = false;
    std::string error;

    Json status() const
    {
        Json result{
            {"connection_id", connection_id},
            {"node_id", endpoint.node_id},
            {"owner_node_id", endpoint.owner_node_id},
            {"service", endpoint.session},
            {"port", endpoint.port},
            {"host", endpoint.endpoint.empty() ? endpoint.host : "localhost"},
            {"remote", endpoint.endpoint.empty()},
            {"online", online},
            {"error", error},
            {"sources", sources}
        };
        result.update(task_page);
        return result;
    }

    Impl(ServiceIo& owner, ServiceEndpoint value)
        : io(owner),
          endpoint(std::move(value))
    {
        validate_id(endpoint.session);
        // Prepare TLS files on the creating management operation before handoff to I/O.
        constexpr std::array<std::size_t, 3> slots{8, 1, 4};
        for (std::size_t i = 0; i < transports.size(); ++i) {
            auto budget = io.payload_budget(
                i == static_cast<std::size_t>(Channel::observation)
                    ? ServiceIo::PayloadLane::control
                    : ServiceIo::PayloadLane::data);
            transports[i] = endpoint.endpoint.empty() ? std::make_unique<ipc::RpcClient>(
                                                            io.reactor(),
                                                            endpoint.host,
                                                            endpoint.port,
                                                            ipc::TlsIdentity{},
                                                            slots[i],
                                                            budget)
                                                      : std::make_unique<ipc::RpcClient>(
                                                            io.reactor(),
                                                            endpoint.endpoint,
                                                            slots[i],
                                                            budget);
        }
    }

    bool stopped(std::stop_token stop) const
    {
        return stop.stop_requested() || lifetime.stop_requested();
    }

    PolledTask<Payload> exchange(
        std::string request,
        std::chrono::milliseconds timeout,
        std::stop_token stop,
        std::chrono::steady_clock::time_point deadline,
        Channel lane = Channel::request)
    {
        auto& transport = transports[static_cast<std::size_t>(lane)];
        if (stopped(stop))
            throw Error(ErrorCode::cancelled, "data connection closed");
        auto reply = transport->request(std::move(request), timeout, deadline);
        while (reply.wait_for(0ms) != std::future_status::ready) {
            if (stopped(stop))
                transport.reset();
            else
                transport->poll();
            co_await std::suspend_always{};
        }
        co_return reply.get();
    }

    void identify(wire::DataRequest& request) const
    {
        request.set_version(1);
        request.set_service_id(endpoint.session);
        request.set_correlation_id(next_correlation_id());
    }

    template <class T, class Decode>
    std::future<T> invoke(
        wire::DataRequest request,
        Decode decode,
        bool large_reply = true,
        std::chrono::milliseconds timeout = 60s,
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max())
    {
        const auto lane = request.has_observation() || request.has_resources()
                || request.has_table_retirement() || request.has_table_retirements()
                || request.has_minute_retirement() || request.has_minute_retirements()
            ? Channel::read
            : Channel::request;
        deadline = std::min(deadline, std::chrono::steady_clock::now() + timeout);
        return io.submit<T>(
            [state = shared_from_this(),
                request = std::move(request),
                decode = std::move(decode),
                large_reply,
                timeout,
                deadline,
                lane](std::stop_token stop) mutable -> PolledTask<T> {
                state->identify(request);
                std::string encoded;
                if (request.has_save_dataset())
                    encoded = co_await state->io.read<std::string>([&] {
                        protocol::validate_named_dataset(request.save_dataset());
                        return request.SerializeAsString();
                    });
                else
                    encoded = request.SerializeAsString();
                auto bytes =
                    co_await state->exchange(std::move(encoded), timeout, stop, deadline, lane);
                auto process = [&]() -> T {
                    auto response = protocol::decode_data_response(request, *bytes);
                    return decode(request, response);
                };
                if (large_reply)
                    co_return co_await state->io.read<T>(process);
                co_return process();
            });
    }

    PolledTask<void> refresh(std::stop_token stop)
    {
        wire::DataRequest request;
        request.mutable_sources();
        identify(request);
        const auto response = protocol::decode_data_response(
            request,
            *(co_await exchange(
                request.SerializeAsString(),
                5s,
                stop,
                std::chrono::steady_clock::now() + 5s,
                Channel::observation)));
        Json declared = Json::array();
        for (const auto& source : response.sources().sources()) {
            declared.push_back(
                {{"id", source.id()},
                    {"name", source.name()},
                    {"plugin_id", source.plugin_id()},
                    {"normalization", source.normalization()},
                    {"timezone", source.timezone()},
                    {"timestamp_semantics", source.timestamp_semantics()},
                    {"venues",
                        std::vector<std::string>(source.venues().begin(), source.venues().end())},
                    {"intervals",
                        std::vector<uint32_t>(
                            source.intervals().begin(),
                            source.intervals().end())},
                    {"max_requests_per_minute", source.max_requests_per_minute()},
                    {"credential_required", source.credential_required()},
                    {"connection",
                        source.has_connection()
                            ? Json{{"credential_label_en",
                                       source.connection().credential_label_en()},
                                  {"credential_label_zh",
                                      source.connection().credential_label_zh()},
                                  {"credential_required",
                                      source.connection().credential_required()},
                                  {"credential_max_length",
                                      source.connection().credential_max_length()},
                                  {"remember_allowed", source.connection().remember_allowed()},
                                  {"requests_per_minute_default",
                                      source.connection().requests_per_minute_default()},
                                  {"requests_per_minute_max",
                                      source.connection().requests_per_minute_max()}}
                            : Json(nullptr)}});
        }
        wire::DataRequest tasks;
        tasks.mutable_acquisition_task()->mutable_list()->set_limit(200);
        tasks.mutable_acquisition_task()->mutable_list()->set_before_sequence(before_sequence);
        identify(tasks);
        auto task_bytes = co_await exchange(
            tasks.SerializeAsString(),
            5s,
            stop,
            std::chrono::steady_clock::now() + 5s,
            Channel::observation);
        auto observed = protocol::decode_data_response(tasks, *task_bytes);
        task_page = co_await io.read<Json>([&] {
            return protocol::decode_task_page(observed.acquisition_task().tasks(), before_sequence);
        });
        sources = std::move(declared);
        typed_sources.assign(
            response.sources().sources().begin(),
            response.sources().sources().end());
        online = true;
        error.clear();
    }

    PolledTask<void> initialize(std::stop_token stop)
    {
        try {
            co_await refresh(stop);
        } catch (const Error& e) {
            error = e.what();
        }
    }

    PolledTask<void> monitor(std::stop_token stop)
    {
        while (!stopped(stop)) {
            const auto next = std::chrono::steady_clock::now() + 1s;
            co_await PollUntil{[&] {
                return stopped(stop) || std::chrono::steady_clock::now() >= next;
            }};
            if (stopped(stop))
                break;
            try {
                co_await refresh(stop);
            } catch (const std::exception& e) {
                online = false;
                error = e.what();
            }
        }
        for (auto& transport : transports)
            transport.reset();
    }
};

DataClient::DataClient(ServiceIo& io, ServiceEndpoint endpoint)
    : impl_(std::make_shared<Impl>(io, std::move(endpoint)))
{}

std::future<std::shared_ptr<DataClient>> DataClient::open(ServiceIo& io, ServiceEndpoint endpoint)
{
    return io.submit<std::shared_ptr<DataClient>>(
        [&io, endpoint = std::move(endpoint)](
            std::stop_token stop) mutable -> PolledTask<std::shared_ptr<DataClient>> {
            auto client = co_await io.admin<std::shared_ptr<DataClient>>([&] {
                return std::shared_ptr<DataClient>(new DataClient(io, std::move(endpoint)));
            });
            co_await client->impl_->initialize(stop);
            (void)client->impl_->io.submit<void>(
                [state = client->impl_](std::stop_token stop) {
                    return state->monitor(stop);
                },
                ServiceIo::Lane::observation);
            co_return client;
        });
}

DataClient::~DataClient()
{
    impl_->lifetime.request_stop();
}

std::future<std::string> DataClient::authorize_download(
    const wire::DownloadAuthorizationRequest& input)
{
    wire::DataRequest request;
    *request.mutable_authorize_download() = input;
    return impl_->invoke<std::string>(
        std::move(request),
        [](const wire::DataRequest& request, wire::DataResponse& response) -> std::string {
            const auto& input = request.authorize_download();
            const auto& authorization = response.download_authorization();
            if (authorization.data_instance() != request.service_id()
                || authorization.task_instance() != input.task_instance()
                || authorization.task_id() != input.task_id())
                throw Error(
                    ErrorCode::unavailable,
                    "download authorization response identity mismatch");
            return authorization.id();
        },
        false,
        120s);
}

std::future<void> DataClient::configure_download_budget(
    const wire::DownloadBudgetConfiguration& input)
{
    wire::DataRequest request;
    *request.mutable_configure_download_budget() = input;
    return impl_->invoke<void>(
        std::move(request),
        [](const wire::DataRequest&, wire::DataResponse&) -> void {},
        false,
        120s);
}

std::future<wire::AcquisitionTaskResponse> DataClient::acquisition_task(
    wire::AcquisitionTaskRequest operation)
{
    wire::DataRequest request;
    *request.mutable_acquisition_task() = std::move(operation);
    return impl_->invoke<wire::AcquisitionTaskResponse>(
        std::move(request),
        [](const wire::DataRequest&, wire::DataResponse& response) {
            return response.acquisition_task();
        });
}

std::future<void>
DataClient::submit_download(const std::string& id, const std::string& authorization)
{
    wire::DataRequest request;
    request.mutable_acquisition_task()->mutable_submit()->set_id(id);
    request.mutable_acquisition_task()->mutable_submit()->set_authorization(authorization);
    return impl_->invoke<void>(
        std::move(request),
        [id](const wire::DataRequest&, wire::DataResponse& response) {
            if (!response.acquisition_task().has_task()
                || response.acquisition_task().task().id() != id)
                throw std::invalid_argument("acquisition response identity mismatch");
        });
}

std::future<Json> DataClient::material_sources()
{
    wire::DataRequest request;
    request.mutable_material_sources();
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest&, wire::DataResponse& response) {
            Json result = Json::array();
            for (const auto& source : response.material_sources().items())
                result.push_back(
                    {{"id", source.id()},
                        {"name", source.name()},
                        {"usage", source.usage()},
                        {"plugin_id", source.plugin_id()},
                        {"provider_artifact", source.provider_artifact()},
                        {"max_bytes", source.max_bytes()},
                        {"credential_required", source.credential_required()},
                        {"no_additional_fee", source.no_additional_fee()}});
            return result;
        });
}

std::future<Json> DataClient::submit_material(const wire::MaterialRequest& material)
{
    protocol::validate_material_request(material);
    wire::DataRequest request;
    *request.mutable_acquisition_task()->mutable_material_submit() = material;
    return impl_->invoke<Json>(
        std::move(request),
        [id = material.id()](const wire::DataRequest&, wire::DataResponse& response) {
            const auto& record = response.acquisition_task().task();
            if (record.id() != id || record.kind() != task::proto::MATERIAL_ACQUISITION)
                throw Error(
                    ErrorCode::unavailable,
                    "material admission response identity mismatch");
            return protocol::decode_task(record);
        });
}

std::future<Json> DataClient::material_read(const std::string& id)
{
    validate_id(id);
    wire::DataRequest request;
    request.mutable_material_read()->set_id(id);
    return impl_->invoke<Json>(
        std::move(request),
        [node = impl_->endpoint.owner_node_id](
            const wire::DataRequest&,
            wire::DataResponse& response) {
            const auto& read = response.material_read();
            if (read.receipt().reference().owner().node_id() != node)
                throw Error(
                    ErrorCode::conflict,
                    "material owner differs from the selected Data node");
            const auto state = read.text().state();
            return Json{
                {"receipt", protocol::decode_material_receipt(read.receipt())},
                {"reading",
                    {{"state",
                         state == wire::MaterialText::READABLE           ? "readable"
                             : state == wire::MaterialText::INVALID_UTF8 ? "invalid_utf8"
                                                                         : "unsupported_media"},
                        {"readable_bytes", read.text().readable_bytes()}}},
                {"text",
                    state == wire::MaterialText::READABLE ? Json(read.original()) : Json(nullptr)}
            };
        });
}

std::future<void> DataClient::page(unsigned before)
{
    return impl_->io.submit<void>(
        [state = impl_, before](std::stop_token stop) -> PolledTask<void> {
            state->before_sequence = before;
            co_await state->refresh(stop);
        });
}

std::future<Json> DataClient::result(const std::string& id)
{
    wire::DataRequest request;
    request.mutable_acquisition_task()->mutable_result()->set_id(id);
    return impl_->invoke<Json>(
        std::move(request),
        [id](const wire::DataRequest&, wire::DataResponse& response) {
            task::proto::TaskResponse result;
            const auto& acquisition = response.acquisition_task();
            *result.mutable_result_task() = acquisition.task();
            if (acquisition.has_minutes())
                *result.mutable_minutes() = acquisition.minutes();
            if (acquisition.has_table_update())
                *result.mutable_table_update() = acquisition.table_update();
            if (acquisition.has_material())
                *result.mutable_material() = acquisition.material();
            return protocol::decode_task_result(result, id);
        });
}

std::future<Json> DataClient::metadata(const std::string& id)
{
    wire::DataRequest request;
    request.mutable_acquisition_task()->mutable_get()->set_id(id);
    return impl_->invoke<Json>(
        std::move(request),
        [id](const wire::DataRequest&, wire::DataResponse& response) {
            if (response.acquisition_task().task().id() != id)
                throw std::invalid_argument("acquisition response identity mismatch");
            return protocol::decode_task(response.acquisition_task().task());
        });
}

std::future<void> DataClient::edit_metadata(const task::proto::TaskMetadataEdit& edit)
{
    wire::DataRequest request;
    *request.mutable_acquisition_task()->mutable_metadata_edit() = edit;
    return impl_->invoke<void>(
        std::move(request),
        [](const wire::DataRequest&, wire::DataResponse&) {});
}

std::future<void> DataClient::action(const std::string& id, const std::string& action)
{
    wire::DataRequest request;
    if (action == "cancel")
        request.mutable_acquisition_task()->mutable_cancel()->set_id(id);
    else if (action == "retry")
        request.mutable_acquisition_task()->mutable_retry()->set_id(id);
    else
        throw std::invalid_argument("unsupported task action");
    return impl_->invoke<void>(
        std::move(request),
        [](const wire::DataRequest&, wire::DataResponse&) {});
}

std::future<Json> DataClient::retirement(const std::string& id, bool execute)
{
    return impl_->io.submit<Json>(
        [state = impl_, id, execute](std::stop_token stop) -> PolledTask<Json> {
            wire::DataRequest request;
            request.mutable_acquisition_task()->mutable_retirement()->set_id(id);
            request.mutable_acquisition_task()->mutable_retirement()->set_execute(execute);
            state->identify(request);
            auto bytes = co_await state->exchange(
                request.SerializeAsString(),
                15s,
                stop,
                std::chrono::steady_clock::now() + 15s);
            auto result = co_await state->io.read<Json>([&] {
                const auto response = protocol::decode_data_response(request, *bytes);
                if (!response.acquisition_task().has_retirement())
                    throw Error(ErrorCode::unavailable, "missing Data retirement response");
                return protocol::decode_task_retirement(
                    response.acquisition_task().retirement(),
                    id,
                    request.service_id(),
                    "data");
            });
            if (result.at("node") != state->endpoint.owner_node_id)
                throw Error(ErrorCode::unavailable, "Data retirement owner identity mismatch");
            if (execute)
                co_await state->refresh(stop);
            co_return result;
        });
}

ServiceEndpoint DataClient::endpoint() const
{
    return impl_->endpoint;
}

Json DataClient::owner_status() const
{
    return impl_->status();
}

std::future<Json> DataClient::status() const
{
    return impl_->io.submit<Json>([state = impl_](std::stop_token) -> PolledTask<Json> {
        co_return state->status();
    });
}

std::future<data::proto::HistorySource> DataClient::source(const std::string& id) const
{
    return impl_->io.submit<data::proto::HistorySource>(
        [state = impl_, id](std::stop_token) -> PolledTask<data::proto::HistorySource> {
            for (const auto& source : state->typed_sources)
                if (source.id() == id)
                    co_return source;
            throw std::invalid_argument("historical data source is unavailable");
        });
}

std::future<std::vector<data::proto::HistorySource>> DataClient::provider_sources(
    const std::string& plugin_id) const
{
    return impl_->io.submit<std::vector<data::proto::HistorySource>>(
        [state = impl_, plugin_id](
            std::stop_token) -> PolledTask<std::vector<data::proto::HistorySource>> {
            std::vector<data::proto::HistorySource> result;
            for (const auto& source : state->typed_sources)
                if (source.plugin_id() == plugin_id && source.has_connection())
                    result.push_back(source);
            co_return result;
        });
}

std::future<data::proto::HistoryConnectionVerification>
DataClient::verify_connection(const std::string& source, const std::string& credential)
{
    wire::DataRequest request;
    request.mutable_verify_connection()->set_source(source);
    request.mutable_verify_connection()->set_credential(credential);
    return impl_->invoke<data::proto::HistoryConnectionVerification>(
        std::move(request),
        [](const wire::DataRequest&,
            wire::DataResponse& response) -> data::proto::HistoryConnectionVerification {
            return std::move(*response.mutable_connection_verification());
        },
        true,
        90s);
}

std::future<data::proto::DominantSeriesPreview> DataClient::dominant_series(
    const std::vector<data::proto::BarDatasetRequest>& months)
{
    wire::DataRequest request;
    for (const auto& month : months)
        *request.mutable_dominant_series()->add_months() = month;
    return impl_->invoke<data::proto::DominantSeriesPreview>(
        std::move(request),
        [](const wire::DataRequest& request,
            wire::DataResponse& response) -> data::proto::DominantSeriesPreview {
            const auto& months = request.dominant_series().months();
            auto& preview = *response.mutable_dominant_series();
            if (preview.months().empty() || preview.months_size() != preview.datasets_size()
                || preview.schedule().rolls_size() != preview.months_size())
                throw Error(ErrorCode::unavailable, "bar dataset identity mismatch");
            for (int i = 0; i < preview.months_size(); ++i) {
                protocol::validate_bar_dataset(preview.datasets(i));
                if (preview.months(i) >= static_cast<unsigned>(months.size())
                    || preview.schedule().rolls(i).contract() != unsigned(i)
                    || protocol::history_identity(preview.datasets(i).contract()).key()
                        != months[preview.months(i)].contract())
                    throw Error(ErrorCode::unavailable, "bar dataset identity mismatch");
            }
            return std::move(preview);
        });
}

std::future<data::proto::DominantSeriesPreview> DataClient::dominant_series(
    const data::proto::ProductBarsRequest& product)
{
    wire::DataRequest request;
    *request.mutable_dominant_series()->mutable_product() = product;
    return impl_->invoke<data::proto::DominantSeriesPreview>(
        std::move(request),
        [](const wire::DataRequest& request,
            wire::DataResponse& response) -> data::proto::DominantSeriesPreview {
            const auto& product = request.dominant_series().product();
            auto& preview = *response.mutable_dominant_series();
            if (preview.months().empty() || preview.months_size() != preview.datasets_size()
                || preview.schedule().rolls_size() != preview.months_size())
                throw Error(ErrorCode::unavailable, "bar dataset identity mismatch");
            for (int i = 0; i < preview.datasets_size(); ++i) {
                const auto& dataset = preview.datasets(i);
                protocol::validate_bar_dataset(dataset);
                // Daily bars of the product asked for, under the snapshot asked for.
                const auto month = protocol::history_identity(dataset.contract());
                if (preview.schedule().rolls(i).contract() != unsigned(i)
                    || dataset.source_dataset_ids_size() || month.venue != product.venue()
                    || month.product != product.product()
                    || dataset.daily().SerializeAsString() != product.daily().SerializeAsString())
                    throw Error(ErrorCode::unavailable, "bar dataset identity mismatch");
            }
            return std::move(preview);
        });
}

std::future<data::proto::BarDataset> DataClient::bar_dataset(
    const data::proto::BarDatasetRequest& query)
{
    wire::DataRequest request;
    *request.mutable_bar_dataset() = query;
    return impl_->invoke<data::proto::BarDataset>(
        std::move(request),
        [](const wire::DataRequest& request,
            wire::DataResponse& response) -> data::proto::BarDataset {
            const auto& query = request.bar_dataset();
            auto& dataset = *response.mutable_bar_dataset();
            protocol::validate_bar_dataset(dataset);
            if (!std::ranges::equal(dataset.source_dataset_ids(), query.source_dataset_ids())
                || dataset.daily().SerializeAsString() != query.daily().SerializeAsString()
                || protocol::history_identity(dataset.contract()).key() != query.contract())
                throw Error(ErrorCode::unavailable, "bar dataset identity mismatch");
            return std::move(dataset);
        });
}

std::future<data::proto::HistoryUpdatePlan> DataClient::history_update_plan(
    const data::proto::HistoryUpdateQuery& query)
{
    wire::DataRequest request;
    *request.mutable_update_plan() = query;
    return impl_->invoke<data::proto::HistoryUpdatePlan>(
        std::move(request),
        [](const wire::DataRequest& request,
            wire::DataResponse& response) -> data::proto::HistoryUpdatePlan {
            const auto& query = request.update_plan();
            auto& plan = *response.mutable_update_plan();
            (void)protocol::decode_history_update_plan(plan);
            if (plan.query().SerializeAsString() != query.SerializeAsString())
                throw Error(ErrorCode::unavailable, "history update plan changed; preview again");
            return std::move(plan);
        });
}

std::future<Json> DataClient::datasets(const data::proto::HistoryFilter& filter)
{
    wire::DataRequest request;
    *request.mutable_datasets() = filter;
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest&, wire::DataResponse& response) -> Json {
            Json items = Json::array();
            for (const auto& row : response.datasets().items())
                items.push_back(
                    {{"id", row.id()},
                        {"contract_id", row.contract_id()},
                        {"source", row.source()},
                        {"revision", row.revision()},
                        {"begin", row.begin()},
                        {"end", row.end()},
                        {"interval_minutes", row.interval_minutes()},
                        {"rows", row.rows()}});
            // Versions this one does not read are kept and not listed.
            return {{"items", std::move(items)}};
        });
}

std::future<Json> DataClient::coverage(const data::proto::HistoryCoverageQuery& query)
{
    wire::DataRequest request;
    *request.mutable_coverage() = query;
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest&, wire::DataResponse& response) -> Json {
            Json items = Json::array();
            for (const auto& row : response.coverage().items()) {
                Json days = Json::array();
                for (const auto& day : row.uncovered_days())
                    days.push_back(day);
                items.push_back(
                    {{"minute_dataset_id", row.minute_dataset_id()},
                        {"minute_source", row.minute_source()},
                        {"interval_minutes", row.interval_minutes()},
                        {"contract_id", row.contract_id()},
                        {"minute_days", row.minute_days()},
                        {"minute_first", row.minute_first()},
                        {"minute_last", row.minute_last()},
                        {"compared_first", row.compared_first()},
                        {"compared_last", row.compared_last()},
                        {"traded_days", row.traded_days()},
                        {"uncovered", row.uncovered()},
                        {"unknown_rows", row.unknown_rows()},
                        {"uncovered_days", std::move(days)}});
            }
            return items;
        });
}

std::future<std::vector<HistoryListing>> DataClient::table_contracts(
    const data::proto::TableContractsQuery& query)
{
    wire::DataRequest request;
    *request.mutable_table_contracts() = query;
    return impl_->invoke<std::vector<HistoryListing>>(
        std::move(request),
        [](const wire::DataRequest& request,
            wire::DataResponse& response) -> std::vector<HistoryListing> {
            const auto& query = request.table_contracts();
            if (response.table_contracts().source() != query.source())
                throw Error(ErrorCode::unavailable, "contract list identity mismatch");
            std::vector<HistoryListing> result;
            for (const auto& row : response.table_contracts().items()) {
                HistoryListing item{
                    HistoryIdentity::parse(row.contract_id()),
                    row.name(),
                    row.list_date(),
                    row.delist_date(),
                    row.source_instrument()
                };
                if (item.identity.venue != query.exchange()
                    || item.identity.product != query.product())
                    throw Error(ErrorCode::unavailable, "contract list identity mismatch");
                (void)parse_trading_date(item.list_date);
                (void)parse_trading_date(item.delist_date);
                if (row.has_multiplier())
                    item.multiplier = Decimal::from_raw(row.multiplier().units());
                if (row.has_per_unit())
                    item.per_unit = Decimal::from_raw(row.per_unit().units());
                if (row.has_trade_unit())
                    item.trade_unit = row.trade_unit();
                if (row.has_quote_unit())
                    item.quote_unit = row.quote_unit();
                result.push_back(std::move(item));
            }
            return result;
        });
}

std::future<Json> DataClient::minute_range(const data::proto::MinuteRangeQuery& query)
{
    wire::DataRequest request;
    *request.mutable_minute_range() = query;
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest& request, wire::DataResponse& response) -> Json {
            if (request.minute_range().SerializeAsString()
                != response.minute_range().query().SerializeAsString())
                throw Error(ErrorCode::unavailable, "minute range identity mismatch");
            return protocol::decode_minute_range_evidence(response.minute_range());
        });
}

std::future<Json> DataClient::minute_evidence(const data::proto::MinuteEvidenceQuery& query)
{
    wire::DataRequest request;
    *request.mutable_minute_evidence() = query;
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest& request, wire::DataResponse& response) -> Json {
            const auto& query = request.minute_evidence();
            const auto& evidence = response.minute_evidence();
            if (evidence.dataset_id() != query.dataset_id() || evidence.limit() != query.limit()
                || evidence.offset() != std::min(query.offset(), evidence.total_days()))
                throw Error(ErrorCode::unavailable, "minute evidence identity mismatch");
            return protocol::decode_minute_evidence(evidence);
        });
}

std::future<Json> DataClient::minute_page(const data::proto::MinutePageQuery& query)
{
    wire::DataRequest request;
    *request.mutable_minute_page() = query;
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest& request, wire::DataResponse& response) -> Json {
            const auto& query = request.minute_page();
            const auto& page = response.minute_page();
            if (page.task_id() != query.dataset_id() || page.offset() != query.offset()
                || page.limit() != query.limit()
                || (query.begin_ns() && page.begin_ns() != query.begin_ns())
                || (query.end_ns() && page.end_ns() != query.end_ns()))
                throw Error(ErrorCode::unavailable, "minute dataset page identity mismatch");
            return protocol::decode_minute_page(page);
        });
}

std::future<data::proto::TableRefreshPlan> DataClient::table_refresh_plan(
    const data::proto::TableRefreshQuery& query)
{
    wire::DataRequest request;
    *request.mutable_table_refresh_plan() = query;
    return impl_->invoke<data::proto::TableRefreshPlan>(
        std::move(request),
        [](const wire::DataRequest& request, wire::DataResponse& response) {
            auto& plan = *response.mutable_table_refresh_plan();
            (void)protocol::decode_table_refresh_plan(plan);
            if (plan.query().SerializeAsString()
                != request.table_refresh_plan().SerializeAsString())
                throw Error(ErrorCode::unavailable, "table refresh plan changed; preview again");
            return std::move(plan);
        });
}

std::future<Json> DataClient::historical_margins(const data::proto::HistoricalMarginQuery& query)
{
    wire::DataRequest request;
    *request.mutable_historical_margins() = query;
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest&, wire::DataResponse& response) {
            return protocol::decode_historical_margins(response.historical_margins());
        });
}

std::future<Json> DataClient::table_coverage()
{
    wire::DataRequest request;
    request.mutable_table_coverage();
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest&, wire::DataResponse& response) -> Json {
            return protocol::decode_table_coverage(response.table_coverage());
        });
}

std::future<Json> DataClient::table_page(const data::proto::TablePageQuery& query)
{
    wire::DataRequest request;
    *request.mutable_table_page() = query;
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest& request, wire::DataResponse& response) -> Json {
            const auto& query = request.table_page();
            const auto& page = response.table_page();
            const auto& held = page.revision();
            if (held.table() != query.partition().table()
                || held.exchange() != query.partition().exchange()
                || held.period() != query.partition().period() || held.source() != query.source()
                || page.offset() != query.offset()
                || page.rows_size() > static_cast<int>(query.limit()))
                throw Error(ErrorCode::unavailable, "history table page identity mismatch");
            return protocol::decode_table_page(page);
        });
}

std::future<Json> DataClient::table_products(const data::proto::TableProductsQuery& query)
{
    wire::DataRequest request;
    *request.mutable_table_products() = query;
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest&, wire::DataResponse& response) -> Json {
            return protocol::decode_table_products(response.table_products());
        });
}

std::future<Json> DataClient::table_listed_contracts(
    const data::proto::TableListedContractsQuery& query)
{
    wire::DataRequest request;
    *request.mutable_table_listed_contracts() = query;
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest& request, wire::DataResponse& response) -> Json {
            const auto& catalog = response.table_listed_contracts();
            if (catalog.query().SerializeAsString()
                != request.table_listed_contracts().SerializeAsString())
                throw Error(ErrorCode::unavailable, "listed contract catalog identity mismatch");
            return protocol::decode_table_listed_contracts(catalog);
        });
}

std::future<Json> DataClient::table_daily(const data::proto::TableDailyQuery& query)
{
    wire::DataRequest request;
    *request.mutable_table_daily() = query;
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest& request, wire::DataResponse& response) -> Json {
            const auto& query = request.table_daily();
            const auto& page = response.table_daily();
            if (page.daily().SerializeAsString() != query.daily().SerializeAsString()
                || page.contract() != query.contract() || page.period() != query.period()
                || page.offset() != query.offset() || page.limit() != query.limit())
                throw Error(ErrorCode::unavailable, "daily table chart identity mismatch");
            return protocol::decode_table_daily(page);
        });
}

std::future<Json>
DataClient::table_retirement(const std::string& id, bool execute, const std::string& preview_digest)
{
    wire::DataRequest request;
    request.mutable_table_retirement()->set_id(id);
    request.mutable_table_retirement()->set_execute(execute);
    request.mutable_table_retirement()->set_preview_digest(preview_digest);
    return impl_->invoke<Json>(
        std::move(request),
        [node = impl_->endpoint.owner_node_id](
            const wire::DataRequest& request,
            wire::DataResponse& response) {
            const auto& value = response.table_retirement();
            if (value.id() != request.table_retirement().id() || value.id().size() != 64
                || value.id().find_first_not_of("0123456789abcdef") != std::string::npos
                || !value.sequence() || value.state() < wire::TableRetirement::AVAILABLE
                || value.state() > wire::TableRetirement::REMOVED || value.objects_size() > 100000
                || value.acquisitions_size() > 10000)
                throw Error(ErrorCode::unavailable, "invalid table retirement response");
            if ((value.state() == wire::TableRetirement::AVAILABLE
                    || value.state() == wire::TableRetirement::PENDING)
                && (value.preview_digest().size() != 64
                    || value.preview_digest().find_first_not_of("0123456789abcdef")
                        != std::string::npos))
                throw Error(ErrorCode::unavailable, "table retirement preview scope is missing");
            const char* state = value.state() == wire::TableRetirement::AVAILABLE ? "available"
                : value.state() == wire::TableRetirement::BLOCKED                 ? "blocked"
                : value.state() == wire::TableRetirement::PENDING                 ? "pending"
                                                                                  : "removed";
            Json result{
                {"id", value.id()},
                {"state", state},
                {"sequence", std::to_string(value.sequence())},
                {"current_head", value.current_head()},
                {"preview_digest", value.preview_digest()},
                {"payload_bytes",
                    value.has_payload_bytes() ? Json(std::to_string(value.payload_bytes()))
                                              : Json(nullptr)},
                {"objects", Json::array()},
                {"acquisitions", Json::array()},
                {"references", Json::array()},
                {"consumers", Json::array()}
            };
            for (const auto& id : value.objects()) {
                if (id.size() != 64
                    || id.find_first_not_of("0123456789abcdef") != std::string::npos)
                    throw Error(ErrorCode::unavailable, "invalid retired table object");
                result["objects"].push_back(id);
            }
            for (const auto& id : value.acquisitions()) {
                validate_id(id);
                result["acquisitions"].push_back(id);
            }
            if (value.has_named_usage()) {
                if (value.named_usage().dataset_id() != value.id())
                    throw Error(ErrorCode::unavailable, "table snapshot usage identity mismatch");
                result["references"] =
                    protocol::decode_history_usage(value.named_usage()).at("references");
            }
            for (const auto& hold : value.holds()) {
                const auto& consumer = hold.retention().consumer();
                if (hold.retention().source().node_id() != node
                    || hold.retention().source().service_id() != request.service_id()
                    || (hold.phase() != wire::DataRetentionRecord::PREPARED
                        && hold.phase() != wire::DataRetentionRecord::CONFIRMED))
                    throw Error(ErrorCode::unavailable, "table retention owner mismatch");
                result["consumers"].push_back(
                    {{"node", consumer.owner().node_id()},
                        {"service", consumer.owner().service_id()},
                        {"domain", consumer.owner().domain()},
                        {"kind", consumer.kind()},
                        {"id", consumer.id()},
                        {"version", consumer.version()},
                        {"phase",
                            hold.phase() == wire::DataRetentionRecord::PREPARED ? "prepared"
                                                                                : "confirmed"}});
            }
            if (value.state() == wire::TableRetirement::AVAILABLE
                && (value.current_head() || value.acquisitions_size() || value.holds_size()
                    || value.named_usage().references_size()))
                throw Error(
                    ErrorCode::unavailable,
                    "table retirement availability contradicts holds");
            return result;
        },
        true,
        120s);
}

std::future<Json> DataClient::table_retirements(const std::string& after)
{
    wire::DataRequest request;
    request.mutable_table_retirements()->set_id(after);
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest& request, wire::DataResponse& response) {
            std::string previous = request.table_retirements().id();
            Json result{
                {"items", Json::array()},
                {"next_after", response.table_retirements().next_after()}
            };
            if (response.table_retirements().items_size() > 256)
                throw Error(ErrorCode::unavailable, "table retirement inventory exceeds capacity");
            for (const auto& item : response.table_retirements().items()) {
                if (item.id() <= previous || item.id().size() != 64
                    || item.id().find_first_not_of("0123456789abcdef") != std::string::npos
                    || !item.sequence()
                    || (item.state() != wire::TableRetirement::PENDING
                        && item.state() != wire::TableRetirement::REMOVED))
                    throw Error(ErrorCode::unavailable, "invalid table retirement inventory");
                result["items"].push_back(
                    {{"id", item.id()},
                        {"sequence", std::to_string(item.sequence())},
                        {"state",
                            item.state() == wire::TableRetirement::PENDING ? "pending"
                                                                           : "removed"}});
                previous = item.id();
            }
            const auto& next = response.table_retirements().next_after();
            if (!next.empty()
                && (response.table_retirements().items_size() != 256 || next != previous))
                throw Error(ErrorCode::unavailable, "invalid table retirement continuation");
            return result;
        });
}

std::future<Json> DataClient::observation(const wire::ObservationQuery& query)
{
    wire::DataRequest request;
    *request.mutable_observation() = query;
    return impl_->invoke<Json>(
        std::move(request),
        [node = impl_->endpoint.owner_node_id](
            const wire::DataRequest& request,
            wire::DataResponse& response) {
            const auto& query = request.observation();
            const auto id = query.has_admit() ? query.admit().id()
                : query.has_execute()         ? query.execute()
                                              : query.read();
            const auto& status = response.observation();
            const auto& operation = status.operation();
            if (operation.id() != id || operation.source().owner().node_id() != node
                || (query.has_admit()
                    && operation.source().SerializeAsString()
                        != query.admit().source().SerializeAsString())
                || (operation.has_archive()
                    && (operation.archive().owner().node_id() != node
                        || operation.archive().owner().service_id() != request.service_id()))
                || (query.has_read() && operation.state() == wire::ObservationOperation::PUBLISHED
                    && !status.has_capture()))
                throw Error(ErrorCode::conflict, "observation archive owner mismatch");
            return protocol::decode_observation_status(status);
        },
        true,
        20s);
}

std::future<Json> DataClient::resources(const resource::proto::CatalogueQuery& query)
{
    wire::DataRequest request;
    *request.mutable_resources() = query;
    return impl_->invoke<Json>(
        std::move(request),
        [node = impl_->endpoint.owner_node_id](
            const wire::DataRequest& request,
            wire::DataResponse& response) {
            resource::proto::Owner owner;
            owner.set_node_id(node);
            owner.set_service_id(request.service_id());
            owner.set_domain("data");
            return protocol::decode_resource_catalogue(
                response.resources(),
                request.resources(),
                owner);
        },
        true,
        30s);
}

std::future<Json> DataClient::minute_retirement(const std::string& id, bool execute)
{
    wire::DataRequest request;
    request.mutable_minute_retirement()->set_id(id);
    request.mutable_minute_retirement()->set_execute(execute);
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest& request, wire::DataResponse& response) {
            if (response.minute_retirement().id() != request.minute_retirement().id())
                throw Error(ErrorCode::unavailable, "minute retirement identity mismatch");
            return retirement_status(response.minute_retirement());
        },
        true,
        120s);
}

std::future<Json> DataClient::minute_retirements(const std::string& after)
{
    wire::DataRequest request;
    request.mutable_minute_retirements()->set_after(after);
    request.mutable_minute_retirements()->set_limit(64);
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest& request, wire::DataResponse& response) {
            const auto& page = response.minute_retirements();
            if (page.items_size() > 64)
                throw Error(ErrorCode::unavailable, "minute retirement page exceeds capacity");
            Json result{{"items", Json::array()}, {"next_after", page.next_after()}};
            std::string previous = request.minute_retirements().after();
            for (const auto& item : page.items()) {
                if (item.id() <= previous
                    || (item.state() != wire::MinuteRetirement::PENDING
                        && item.state() != wire::MinuteRetirement::REMOVED))
                    throw Error(ErrorCode::unavailable, "invalid minute retirement page");
                result["items"].push_back(retirement_status(item));
                previous = item.id();
            }
            if (!page.next_after().empty()
                && (page.items_size() != 64 || page.next_after() != previous))
                throw Error(ErrorCode::unavailable, "invalid minute retirement continuation");
            return result;
        });
}

std::future<Json> DataClient::history_usage(const std::string& id)
{
    wire::DataRequest request;
    request.mutable_history_usage()->set_id(id);
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest& request, wire::DataResponse& response) -> Json {
            const auto& id = request.history_usage().id();
            if (response.history_usage().dataset_id() != id)
                throw Error(ErrorCode::unavailable, "invalid historical usage response");
            return protocol::decode_history_usage(response.history_usage());
        });
}

std::future<Json> DataClient::inspect_history_usage(
    ServiceIo& io,
    const ServiceEndpoint& endpoint,
    const std::string& id,
    std::chrono::steady_clock::time_point deadline)
{
    auto state = std::make_shared<Impl>(io, endpoint);
    wire::DataRequest request;
    request.mutable_history_usage()->set_id(id);
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining <= 0ms)
        throw Error(ErrorCode::unavailable, "data request timed out");
    return state->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest& request, wire::DataResponse& response) {
            if (response.history_usage().dataset_id() != request.history_usage().id())
                throw Error(ErrorCode::unavailable, "invalid historical usage response");
            return protocol::decode_history_usage(response.history_usage());
        },
        true,
        remaining,
        deadline);
}

std::future<Json> DataClient::saved_datasets()
{
    wire::DataRequest request;
    request.mutable_saved_datasets();
    return impl_->invoke<Json>(
        std::move(request),
        [](const wire::DataRequest&, wire::DataResponse& response) -> Json {
            Json items = Json::array();
            for (const auto& item : response.saved_datasets().items())
                items.push_back(protocol::decode_named_dataset(item));
            return {{"items", std::move(items)}};
        });
}

std::future<data::proto::NamedDataset> DataClient::saved_dataset(const std::string& id)
{
    wire::DataRequest request;
    request.mutable_saved_dataset()->set_id(id);
    return impl_->invoke<data::proto::NamedDataset>(
        std::move(request),
        [](const wire::DataRequest& request,
            wire::DataResponse& response) -> data::proto::NamedDataset {
            auto& value = *response.mutable_saved_dataset();
            protocol::validate_named_dataset(value);
            if (value.id() != request.saved_dataset().id())
                throw std::invalid_argument("saved dataset revision mismatch");
            return std::move(value);
        });
}

std::future<void> DataClient::save_dataset(const data::proto::NamedDataset& value)
{
    wire::DataRequest request;
    *request.mutable_save_dataset() = value;
    return impl_->invoke<void>(
        std::move(request),
        [](const wire::DataRequest& request, wire::DataResponse& response) -> void {
            const auto& value = request.save_dataset();
            if (response.saved_dataset().SerializeAsString() != value.SerializeAsString())
                throw std::invalid_argument("saved dataset revision mismatch");
        });
}

} // namespace asterion::terminal
