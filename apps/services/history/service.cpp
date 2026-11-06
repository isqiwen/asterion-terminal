#include "service.hpp"
#include "acquisition/exchanges.hpp"
#include "acquisition/guotai.hpp"
#include "acquisition/catalog_source.hpp"
#include "acquisition/sina.hpp"
#include "acquisition/snapshot_intake.hpp"
#include "acquisition/tushare.hpp"
#include "warehouse/composer.hpp"
#include "warehouse/reader.hpp"
#include "warehouse/research.hpp"
#include <asterion/version.hpp>
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/system/log.hpp>
#include <thread>

namespace asterion::history {

using namespace std::chrono_literals;
namespace wire = ::history;
using Clock = std::chrono::steady_clock;
using Message = service::RpcHost::Message;
using Reply = service::RpcHost::Reply;

namespace {

// Where a call is answered and whether it needs the `manage` permission.
enum class Lane
{
    // Quick lookups and file chunks.
    reader,
    // Scans that can take long, on their own thread so lookups never wait for them.
    analysis
};

struct Route
{
    Lane lane;
    bool manage = false;
};

Route route(const wire::Request& request)
{
    switch (request.call_case()) {
        case wire::Request::kStartAcquisition:
        case wire::Request::kCancelAcquisition:
            return {Lane::reader, true};
        case wire::Request::kGetDatasetSummary:
        case wire::Request::kGetDatasetCoverage:
        case wire::Request::kGetTableRows:
        case wire::Request::kListDatasetVersions:
        case wire::Request::kCompareDatasetVersions:
            return {Lane::analysis};
        default:
            return {Lane::reader};
    }
}

// Only `Error` and invalid-request messages reach the client; anything else is
// logged and reported generically.
void fail(wire::Response& response, const std::exception& error)
{
    *response.mutable_error() = protocol::error(error);
    if (!dynamic_cast<const Error*>(&error) && classify(error) != ErrorCode::invalid_request) {
        log::warn("market request {} failed: {}", response.id(), error.what());
        response.mutable_error()->set_message("History request failed; check service logs");
    }
}

template <class Fill>
wire::Response respond(const wire::Request& request, Fill&& fill)
{
    wire::Response response;
    response.set_id(request.id());
    try {
        fill(response);
    } catch (const std::exception& error) {
        fail(response, error);
    }
    protocol::log_rpc_result("history", request, response, !route(request).manage);
    return response;
}

Message reply(const wire::Response& response)
{
    return {response.SerializeAsString()};
}

Reply ready(Message message)
{
    return [message = std::make_shared<std::optional<Message>>(std::move(message))] {
        return std::exchange(*message, std::nullopt);
    };
}

Reply waiting(std::future<Message> result, std::uint64_t id)
{
    return [result = std::make_shared<std::future<Message>>(std::move(result)),
               id]() -> std::optional<Message> {
        if (result->wait_for(0ms) != std::future_status::ready)
            return {};
        try {
            return result->get();
        } catch (const std::exception& error) {
            // The market thread stopped before it answered.
            wire::Response response;
            response.set_id(id);
            *response.mutable_error() = protocol::error(error);
            return reply(response);
        }
    };
}

std::map<std::string, acquisition::Factory> sources(const Settings& settings)
{
    std::map<std::string, acquisition::Factory> result;
    result["ctp"] = [root = settings.snapshots](std::stop_token stop) {
        return std::make_unique<acquisition::CatalogSource>(root, stop);
    };
    std::vector<std::string> reference_exchanges;
    for (const auto& exchange : settings.exchanges) {
        auto id = exchange;
        std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) {
            return std::tolower(c);
        });
        result[id] = [exchange](std::stop_token stop) {
            return std::make_unique<acquisition::Exchange>(exchange, stop);
        };
        if (exchange != "CZCE")
            reference_exchanges.push_back(exchange);
    }
    if (!settings.exchanges.empty()) {
        // Sina answers by contract and is known to block heavy use.
        auto budget = std::make_shared<acquisition::Budget>(60);
        result["sina"] = [exchanges = settings.exchanges, root = settings.snapshots, budget](
                             std::stop_token stop) {
            return std::make_unique<acquisition::Sina>(exchanges, root, budget, stop);
        };
    }
    if (!reference_exchanges.empty())
        result["gtja"] = [reference_exchanges](std::stop_token stop) {
            return std::make_unique<acquisition::Guotai>(reference_exchanges, stop);
        };
    if (!settings.tushare_token.empty()) {
        auto budget = std::make_shared<acquisition::Budget>(settings.tushare_requests_per_minute);
        result["tushare"] = [token = settings.tushare_token, budget](std::stop_token stop) {
            return std::make_unique<acquisition::Tushare>(token, budget, stop);
        };
    }
    return result;
}

void answer_reader(
    const wire::Request& request,
    wire::Response& response,
    const std::filesystem::path& directory,
    acquisition::Manager& tasks,
    std::int64_t started_ms,
    bool can_manage)
{
    const warehouse::Reader datasets(directory);
    switch (request.call_case()) {
        case wire::Request::kGetStatus: {
            auto* status = response.mutable_get_status();
            status->set_version(asterion::build::description);
            status->set_started_ms(started_ms);
            status->set_disk_free_bytes(std::filesystem::space(directory).available);
            break;
        }
        case wire::Request::kGetDatasetVersion:
            *response.mutable_get_dataset_version() =
                datasets.version(request.get_dataset_version());
            break;
        case wire::Request::kReadDatasetFile:
            *response.mutable_read_dataset_file() = datasets.file(request.read_dataset_file());
            break;
        case wire::Request::kGetDatasetSummary:
            *response.mutable_get_dataset_summary() =
                datasets.summary(request.get_dataset_summary());
            break;
        case wire::Request::kGetDatasetCoverage:
            *response.mutable_get_dataset_coverage() =
                datasets.coverage(request.get_dataset_coverage());
            break;
        case wire::Request::kGetTableRows:
            *response.mutable_get_table_rows() = datasets.rows(request.get_table_rows());
            break;
        case wire::Request::kListDatasetVersions:
            *response.mutable_list_dataset_versions() =
                datasets.versions(request.list_dataset_versions());
            break;
        case wire::Request::kCompareDatasetVersions:
            *response.mutable_compare_dataset_versions() =
                datasets.change(request.compare_dataset_versions());
            break;
        case wire::Request::kGetAcquisitionOptions:
            *response.mutable_get_acquisition_options() = tasks.options();
            response.mutable_get_acquisition_options()->set_can_manage(can_manage);
            break;
        case wire::Request::kStartAcquisition:
            *response.mutable_start_acquisition() = tasks.submit(request.start_acquisition());
            break;
        case wire::Request::kGetAcquisitionTask:
            *response.mutable_get_acquisition_task() =
                tasks.get(request.get_acquisition_task().id());
            break;
        case wire::Request::kListAcquisitionTasks:
            *response.mutable_list_acquisition_tasks() =
                tasks.list(request.list_acquisition_tasks());
            break;
        case wire::Request::kCancelAcquisition:
            tasks.cancel(request.cancel_acquisition().id());
            *response.mutable_cancel_acquisition() = tasks.get(request.cancel_acquisition().id());
            break;
        default:
            break;
    }
}

} // namespace

struct HistoryService::Running
{
    const Settings settings;
    const std::int64_t started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
    warehouse::Composer composer;
    acquisition::Manager acquisitions;
    acquisition::SnapshotIntake intake;
    ThreadPool readers{1, 8}, analysis{1, 8};
    std::promise<void> completion;
    std::shared_future<void> stopped = completion.get_future().share();
    std::jthread closing;

    explicit Running(Settings settings)
        : settings(std::move(settings)),
          composer(this->settings.directory),
          acquisitions(
              this->settings.directory,
              sources(this->settings),
              [this] {
                  composer.request();
              }),
          intake(this->settings.snapshots, this->settings.directory, acquisitions)
    {}

    Reply ask(ThreadPool& pool, wire::Request request, bool can_manage)
    {
        const auto id = request.id();
        auto promise = std::make_shared<std::promise<Message>>();
        auto result = promise->get_future();
        pool.submit([this, request = std::move(request), promise, can_manage](std::stop_token) {
            promise->set_value(reply(respond(request, [&](wire::Response& out) {
                answer_reader(
                    request,
                    out,
                    settings.directory,
                    acquisitions,
                    started_ms,
                    can_manage);
            })));
            service::wake_io_owner();
        });
        return waiting(std::move(result), id);
    }
};

HistoryService::HistoryService(Settings settings)
    : running_(std::make_unique<Running>(std::move(settings)))
{}

HistoryService::~HistoryService() = default;

HistoryService::Reply HistoryService::serve(std::string frame, std::optional<bool> can_manage)
{
    wire::Request request;
    try {
        const bool parsed = request.ParseFromString(frame);
        std::fill(frame.begin(), frame.end(), '\0');
        if (!parsed)
            throw std::invalid_argument("malformed Protobuf request");
        protocol::validate_message(request);
        if (request.call_case() == wire::Request::CALL_NOT_SET)
            throw std::invalid_argument("missing market operation");
        const auto [lane, manage] = route(request);
        if (!can_manage || (manage && !*can_manage))
            throw Error(
                ErrorCode::permission_denied,
                "History acquisition management permission is required");
        return running_->ask(
            lane == Lane::analysis ? running_->analysis : running_->readers,
            std::move(request),
            *can_manage);
    } catch (const std::exception& error) {
        wire::Response response;
        response.set_id(request.id());
        fail(response, error);
        protocol::log_rpc_result("history", request, response);
        return ready(reply(response));
    }
}

void HistoryService::stop()
{
    if (running_->closing.joinable())
        return;
    // Tasks may take a while to notice; the I/O owner keeps serving meanwhile.
    running_->closing = std::jthread([running = running_.get()] {
        running->intake.stop();
        running->acquisitions.stop();
        running->composer.stop();
        running->completion.set_value();
    });
}

std::shared_future<void> HistoryService::stopped() const
{
    return running_->stopped;
}

} // namespace asterion::history
