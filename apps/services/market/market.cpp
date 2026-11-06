#include "market.hpp"
#include "live.hpp"
#include "recording.hpp"
#include "acquisition/exchanges.hpp"
#include "acquisition/guotai.hpp"
#include "acquisition/recorded.hpp"
#include "acquisition/recorded_intake.hpp"
#include "acquisition/tushare.hpp"
#include "recorded/compaction.hpp"
#include "recorded/reader.hpp"
#include "warehouse/composer.hpp"
#include "warehouse/reader.hpp"
#include "warehouse/research.hpp"
#include <asterion/base/bounded_queue.hpp>
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <asterion/system/log.hpp>
#include <thread>

namespace asterion::market_data {

using namespace std::chrono_literals;
namespace wire = ::market;
using Clock = std::chrono::steady_clock;
using Message = service::RpcHost::Message;
using Reply = service::RpcHost::Reply;

namespace {

// Where a call is answered and whether it needs the `manage` permission.
enum class Lane
{
    // From the live market, on the market thread.
    market,
    // A frame every 250 ms from the market thread until the connection closes.
    stream,
    // From files, on the reader pool.
    reader
};

struct Route
{
    Lane lane;
    bool manage = false;
};

Route route(const wire::Request& request)
{
    switch (request.call_case()) {
        case wire::Request::kGetStatus:
        case wire::Request::kGetBoard:
        case wire::Request::kGetMinutes:
            return {Lane::market};
        case wire::Request::kStreamQuotes:
            return {Lane::stream};
        case wire::Request::kStartAcquisition:
        case wire::Request::kCancelAcquisition:
            return {Lane::reader, true};
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
        response.mutable_error()->set_message("Market request failed; check service logs");
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
    protocol::log_rpc_result("market", request, response, !route(request).manage);
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

// An open stream_quotes reply: the market thread leaves each frame here and
// the connection takes it.
struct QuoteStream
{
    explicit QuoteStream(wire::Request request)
        : request(std::move(request))
    {}

    const wire::Request request;
    // Market thread only.
    Sent sent;
    Clock::time_point due{};

    bool waiting() const
    {
        std::lock_guard lock(mutex);
        return frame.has_value();
    }

    void put(std::string payload, bool more)
    {
        std::lock_guard lock(mutex);
        frame.emplace(std::move(payload), more);
    }

    std::optional<Message> take(bool last)
    {
        std::lock_guard lock(mutex);
        auto taken = std::exchange(frame, std::nullopt);
        if (taken && last)
            taken->more = false;
        return taken;
    }

private:
    mutable std::mutex mutex;
    std::optional<Message> frame;
};

std::map<std::string, acquisition::Factory>
sources(const Settings& settings, std::shared_ptr<std::shared_mutex> archive)
{
    std::map<std::string, acquisition::Factory> result;
    result["recorder"] = [root = settings.directory / "recordings", archive](std::stop_token stop) {
        return std::make_unique<acquisition::Recorded>(root, archive, stop);
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

void answer_live(
    const wire::Request& request,
    wire::Response& response,
    const Live& live,
    const Recording& recording,
    const acquisition::RecordedIntake& intake)
{
    switch (request.call_case()) {
        case wire::Request::kGetStatus:
            live.status(*response.mutable_get_status());
            recording.status(*response.mutable_get_status()->mutable_recording());
            {
                const auto merging = intake.status();
                auto* out = response.mutable_get_status()->mutable_recording();
                out->set_merging_day(protocol::iso_day(merging.day));
                out->set_merge_error(merging.error);
            }
            break;
        case wire::Request::kGetBoard: {
            Sent nothing;
            *response.mutable_get_board() = std::move(*live.frame({}, nothing).mutable_full());
            break;
        }
        case wire::Request::kGetMinutes:
            *response.mutable_get_minutes() = live.minutes(request.get_minutes());
            break;
        default:
            break;
    }
}

// Days arrive as YYYY-MM-DD; the archive names them YYYYMMDD.
wire::RecordedDays recorded_days(
    const std::filesystem::path& recordings,
    std::shared_mutex& archive,
    const wire::ListRecordedDays& query)
{
    warehouse::require_day(query.first_day());
    warehouse::require_day(query.last_day());
    const auto compact = [](std::string day) {
        std::erase(day, '-');
        return day;
    };
    // A merge replaces archive files; read between merges.
    std::shared_lock lock(archive);
    recording::Reader reader(recordings);
    wire::RecordedDays result;
    for (const auto& day : reader.days(compact(query.first_day()), compact(query.last_day()))) {
        auto* out = result.add_days();
        out->set_trading_day(protocol::iso_day(day));
        for (const auto& segment : reader.segments(day)) {
            auto* item = out->add_segments();
            item->set_instance(segment.instance);
            item->set_number(segment.number);
            item->set_first_received_ms(segment.first_received_ms);
            item->set_last_received_ms(segment.last_received_ms);
            item->set_ticks(segment.ticks);
            item->set_end(segment.end);
        }
    }
    return result;
}

void answer_reader(
    const wire::Request& request,
    wire::Response& response,
    const std::filesystem::path& directory,
    std::shared_mutex& archive,
    acquisition::Manager& tasks,
    bool can_manage)
{
    const warehouse::Reader datasets(directory);
    switch (request.call_case()) {
        case wire::Request::kListDatasets:
            *response.mutable_list_datasets() = datasets.datasets();
            break;
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
        case wire::Request::kCompareMinutes:
            *response.mutable_compare_minutes() = warehouse::compare_minutes(directory);
            break;
        case wire::Request::kGetMinuteDifferences:
            *response.mutable_get_minute_differences() =
                warehouse::minute_differences(directory, request.get_minute_differences());
            break;
        case wire::Request::kGetTableRows:
            *response.mutable_get_table_rows() = datasets.rows(request.get_table_rows());
            break;
        case wire::Request::kListRecordedDays:
            *response.mutable_list_recorded_days() =
                recorded_days(directory / "recordings", archive, request.list_recorded_days());
            break;
        case wire::Request::kListDatasetVersions:
            *response.mutable_list_dataset_versions() =
                datasets.versions(request.list_dataset_versions());
            break;
        case wire::Request::kCompareDatasetVersions:
            *response.mutable_compare_dataset_versions() =
                datasets.change(request.compare_dataset_versions());
            break;
        case wire::Request::kListSources:
            *response.mutable_list_sources() = tasks.sources();
            response.mutable_list_sources()->set_can_manage(can_manage);
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

struct Market::Running
{
    using Call = std::packaged_task<Message(const Live&, const Recording&)>;

    const Settings settings;
    const std::string instance;
    const std::shared_ptr<std::shared_mutex> archive = std::make_shared<std::shared_mutex>();
    warehouse::Composer composer;
    acquisition::Manager acquisitions;
    acquisition::RecordedIntake intake;
    ThreadPool readers{1, 8};
    BoundedQueue<Call> calls{64};
    std::mutex streams_mutex;
    std::vector<std::weak_ptr<QuoteStream>> streams;
    std::atomic<bool> draining = false;
    std::promise<void> completion;
    std::shared_future<void> stopped = completion.get_future().share();
    std::jthread thread;

    Running(Settings settings, std::string instance)
        : settings(std::move(settings)),
          instance(std::move(instance)),
          composer(this->settings.directory),
          acquisitions(
              this->settings.directory,
              sources(this->settings, archive),
              [this] {
                  composer.request();
              }),
          intake(
              this->settings.directory / "recordings",
              archive,
              this->settings.tick_days,
              acquisitions)
    {
        thread = std::jthread([this](std::stop_token stop) {
            try {
                run(stop);
                // Recording closed with run(); only now may history take its time.
                intake.stop();
                acquisitions.stop();
                composer.stop();
                completion.set_value();
            } catch (...) {
                completion.set_exception(std::current_exception());
            }
            calls.close();
        });
    }

    void run(std::stop_token stop)
    {
        Live live(settings);
        Recording recording(settings.directory, instance, live.minutes());
        while (!stop.stop_requested()) {
            const auto now = Clock::now();
            live.maintain(now);
            const auto update = live.poll();
            for (const auto& event : update.events) {
                recording.observe(event);
                live.apply(event);
            }
            // A feed that lost callbacks never recovers in place: exit, and
            // systemd starts a complete stream again.
            if (update.failed) {
                recording.interrupt("feed_failed");
                throw std::runtime_error("market feed lost provider events");
            }
            recording.maintain();
            bool answered = false;
            while (auto call = calls.try_pop()) {
                (*call)(live, recording);
                answered = true;
            }
            if (publish(live, now) || answered)
                service::wake_io_owner();
            std::this_thread::sleep_for(10ms);
        }
    }

    // Leaves the next frame for each stream that has taken its last one and is due.
    bool publish(const Live& live, Clock::time_point now)
    {
        std::vector<std::shared_ptr<QuoteStream>> open;
        {
            std::lock_guard lock(streams_mutex);
            std::erase_if(streams, [](const auto& stream) {
                return stream.expired();
            });
            for (const auto& weak : streams)
                if (auto stream = weak.lock())
                    open.push_back(std::move(stream));
        }
        bool published = false;
        for (const auto& stream : open) {
            if (stream->waiting() || (now < stream->due && !draining))
                continue;
            auto response = respond(stream->request, [&](wire::Response& out) {
                *out.mutable_stream_quotes() =
                    live.frame(stream->request.stream_quotes(), stream->sent);
            });
            stream->put(response.SerializeAsString(), response.has_stream_quotes());
            stream->due = now + 250ms;
            published = true;
        }
        return published;
    }

    Reply open_stream(wire::Request request)
    {
        std::vector<InstrumentId> ids;
        for (const auto& id : request.stream_quotes().instruments())
            ids.push_back(protocol::instrument(id));
        if (ids.size() > 50)
            throw std::invalid_argument("at most 50 watch contracts");
        ctp::validate_instruments(ids);
        auto stream = std::make_shared<QuoteStream>(std::move(request));
        {
            std::lock_guard lock(streams_mutex);
            std::erase_if(streams, [](const auto& stream) {
                return stream.expired();
            });
            if (streams.size() == 32)
                throw Error(ErrorCode::resource_exhausted, "market watch capacity is full");
            streams.push_back(stream);
        }
        return [this, stream] {
            return stream->take(draining);
        };
    }

    Reply ask_market_thread(wire::Request request)
    {
        const auto id = request.id();
        Call call(
            [this, request = std::move(request)](const Live& live, const Recording& recording) {
                return reply(respond(request, [&](wire::Response& out) {
                    answer_live(request, out, live, recording, intake);
                }));
            });
        auto result = call.get_future();
        if (!calls.try_push(std::move(call)))
            throw Error(ErrorCode::resource_exhausted, "market request queue is full or stopped");
        return waiting(std::move(result), id);
    }

    Reply ask_readers(wire::Request request, bool can_manage)
    {
        const auto id = request.id();
        auto promise = std::make_shared<std::promise<Message>>();
        auto result = promise->get_future();
        readers.submit([this, request = std::move(request), promise, can_manage](std::stop_token) {
            promise->set_value(reply(respond(request, [&](wire::Response& out) {
                answer_reader(request, out, settings.directory, *archive, acquisitions, can_manage);
            })));
            service::wake_io_owner();
        });
        return waiting(std::move(result), id);
    }
};

Market::Market(Settings settings, std::string instance)
    : running_(std::make_unique<Running>(std::move(settings), std::move(instance)))
{}

Market::~Market() = default;

Market::Reply Market::serve(std::string frame, std::optional<bool> can_manage)
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
                "Market acquisition management permission is required");
        switch (lane) {
            case Lane::market:
                return running_->ask_market_thread(std::move(request));
            case Lane::stream:
                return running_->open_stream(std::move(request));
            case Lane::reader:
                return running_->ask_readers(std::move(request), *can_manage);
        }
    } catch (const std::exception& error) {
        wire::Response response;
        response.set_id(request.id());
        fail(response, error);
        protocol::log_rpc_result("market", request, response);
        return ready(reply(response));
    }
    return {};
}

void Market::drain()
{
    running_->draining = true;
}

void Market::stop()
{
    running_->calls.close();
    running_->thread.request_stop();
}

std::shared_future<void> Market::stopped() const
{
    return running_->stopped;
}

} // namespace asterion::market_data
