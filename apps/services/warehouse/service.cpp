#include "service.hpp"
#include "acquisition/exchanges.hpp"
#include "acquisition/guotai.hpp"
#include "acquisition/manager.hpp"
#include "acquisition/sina.hpp"
#include "acquisition/tushare.hpp"
#include "acquisition/update.hpp"
#include "composer.hpp"
#include "reader.hpp"
#include "composition.hpp"
#include "freshness.hpp"
#include <asterion/version.hpp>
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/system/log.hpp>
#include <condition_variable>
#include <thread>

namespace asterion::warehouse {

using namespace std::chrono_literals;
namespace wire = ::warehouse;
using Clock = std::chrono::steady_clock;
using Message = service::RpcHost::Message;
using Reply = service::RpcHost::Reply;

namespace {

// Where a call is answered.
enum class Lane
{
    // Quick lookups and file chunks.
    reader,
    // Scans that can take long, on their own thread so lookups never wait for them.
    analysis
};

Lane lane(const wire::Request& request)
{
    switch (request.call_case()) {
        case wire::Request::kGetDatasetSummary:
        case wire::Request::kGetDatasetCoverage:
        case wire::Request::kGetTableRows:
        case wire::Request::kListDatasetVersions:
        case wire::Request::kCompareDatasetVersions:
        case wire::Request::kStartUpdate:
            return Lane::analysis;
        default:
            return Lane::reader;
    }
}

bool routine(const wire::Request& request)
{
    return request.call_case() != wire::Request::kStartAcquisition
        && request.call_case() != wire::Request::kCancelAcquisition
        && request.call_case() != wire::Request::kStartUpdate;
}

// Only `Error` and invalid-request messages reach the client; anything else is
// logged and reported generically.
void fail(wire::Response& response, const std::exception& error)
{
    *response.mutable_error() = protocol::error(error);
    if (!dynamic_cast<const Error*>(&error) && classify(error) != ErrorCode::invalid_request) {
        log::warn("market request {} failed: {}", response.id(), error.what());
        response.mutable_error()->set_message("Warehouse request failed; check service logs");
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
    protocol::log_rpc_result("warehouse", request, response, routine(request));
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
        result["sina"] = [exchanges = settings.exchanges, root = settings.directory, budget](
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

// Submits the update's acquisitions, leaving out tables with a task in progress.
wire::AcquisitionTasks update(const std::filesystem::path& directory, acquisition::Manager& tasks)
{
    // The daily update and a client's can coincide; neither may add a table's task twice.
    static std::mutex updating;
    std::lock_guard lock(updating);
    const auto prefix = "update-"
        + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    wire::AcquisitionTasks result;
    for (auto& request : acquisition::update_requests(directory, yesterday(), prefix))
        if (!tasks.busy(request.table()))
            *result.add_tasks() = tasks.submit(std::move(request));
    return result;
}

// The next time of day `at` in Beijing after now.
std::chrono::system_clock::time_point next(std::chrono::minutes at)
{
    const auto now = std::chrono::system_clock::now();
    const auto midnight = std::chrono::floor<std::chrono::days>(now + 8h) - 8h;
    return midnight + at > now ? midnight + at : midnight + std::chrono::days{1} + at;
}

void answer_reader(
    const wire::Request& request,
    wire::Response& response,
    const std::filesystem::path& directory,
    acquisition::Manager& tasks,
    std::int64_t started_ms)
{
    const warehouse::Reader datasets(directory);
    switch (request.call_case()) {
        case wire::Request::kGetStatus: {
            auto* status = response.mutable_get_status();
            status->set_version(asterion::build::description);
            status->set_protocol(asterion::build::warehouse_protocol);
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
        case wire::Request::kStartUpdate:
            *response.mutable_start_update() = update(directory, tasks);
            break;
        default:
            break;
    }
}

} // namespace

struct WarehouseService::Running
{
    explicit Running(Settings settings)
        : settings(std::move(settings)),
          composer(this->settings.directory),
          acquisitions(
              this->settings.directory,
              sources(this->settings),
              [this](const auto& committed) {
                  composer.request(committed);
              })
    {
        if (this->settings.update_at)
            daily = std::jthread([this](std::stop_token stop) {
                std::mutex mutex;
                std::condition_variable_any wake;
                std::unique_lock lock(mutex);
                while (!wake.wait_until(lock, stop, next(*this->settings.update_at), [] {
                    return false;
                }) && !stop.stop_requested())
                    try {
                        const auto tasks = update(this->settings.directory, acquisitions);
                        log::info("daily update submitted {} tasks", tasks.tasks_size());
                    } catch (const std::exception& error) {
                        log::error("daily update failed: {}", error.what());
                    }
            });
    }

    Reply ask(ThreadPool& pool, wire::Request request)
    {
        const auto id = request.id();
        auto promise = std::make_shared<std::promise<Message>>();
        auto result = promise->get_future();
        pool.submit([this, request = std::move(request), promise](std::stop_token) {
            promise->set_value(reply(respond(request, [&](wire::Response& out) {
                answer_reader(request, out, settings.directory, acquisitions, started_ms);
            })));
            service::wake_io_owner();
        });
        return waiting(std::move(result), id);
    }

    const Settings settings;
    const std::int64_t started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
    warehouse::Composer composer;
    acquisition::Manager acquisitions;
    ThreadPool readers{1, 8}, analysis{1, 8};
    std::promise<void> completion;
    std::shared_future<void> stopped = completion.get_future().share();
    std::jthread closing;
    // Last, so it stops before the acquisitions it submits to.
    std::jthread daily;
};

WarehouseService::WarehouseService(Settings settings)
    : running_(std::make_unique<Running>(std::move(settings)))
{}

WarehouseService::~WarehouseService() = default;

WarehouseService::Reply WarehouseService::serve(std::string frame, bool known)
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
        if (!known)
            throw Error(ErrorCode::permission_denied, "this client is not registered");
        return running_->ask(
            lane(request) == Lane::analysis ? running_->analysis : running_->readers,
            std::move(request));
    } catch (const std::exception& error) {
        wire::Response response;
        response.set_id(request.id());
        fail(response, error);
        protocol::log_rpc_result("warehouse", request, response);
        return ready(reply(response));
    }
}

void WarehouseService::stop()
{
    if (running_->closing.joinable())
        return;
    // Tasks may take a while to notice; the I/O owner keeps serving meanwhile.
    running_->daily.request_stop();
    running_->closing = std::jthread([running = running_.get()] {
        running->acquisitions.stop();
        running->composer.stop();
        running->completion.set_value();
    });
}

std::shared_future<void> WarehouseService::stopped() const
{
    return running_->stopped;
}

} // namespace asterion::warehouse
