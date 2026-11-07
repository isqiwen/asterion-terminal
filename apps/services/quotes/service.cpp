#include "service.hpp"
#include "live.hpp"
#include <asterion/base/bounded_queue.hpp>
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <asterion/system/log.hpp>
#include <thread>

namespace asterion::quotes {

using namespace std::chrono_literals;
namespace wire = ::quotes;
using Clock = std::chrono::steady_clock;
using Message = service::RpcHost::Message;
using Reply = service::RpcHost::Reply;

namespace {

// `get_status`, `get_board` and `get_minutes` are answered on the market thread;
// `stream_quotes` gets a frame from it every 250 ms until the connection closes.
// Only `Error` and invalid-request messages reach the client; anything else is
// logged and reported generically.
void fail(wire::Response& response, const std::exception& error)
{
    *response.mutable_error() = protocol::error(error);
    if (!dynamic_cast<const Error*>(&error) && classify(error) != ErrorCode::invalid_request) {
        log::warn("market request {} failed: {}", response.id(), error.what());
        response.mutable_error()->set_message("Quotes request failed; check service logs");
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
    protocol::log_rpc_result("quotes", request, response, true);
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

void answer_live(const wire::Request& request, wire::Response& response, const Live& live)
{
    switch (request.call_case()) {
        case wire::Request::kGetStatus:
            live.status(*response.mutable_get_status());
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

} // namespace

struct QuotesService::Running
{
    using Call = std::packaged_task<Message(const Live&)>;

    const Settings settings;
    BoundedQueue<Call> calls{64};
    std::mutex streams_mutex;
    std::vector<std::weak_ptr<QuoteStream>> streams;
    std::atomic<bool> draining = false;
    std::promise<void> completion;
    std::shared_future<void> stopped = completion.get_future().share();
    std::jthread thread;

    explicit Running(Settings settings)
        : settings(std::move(settings))
    {
        thread = std::jthread([this](std::stop_token stop) {
            try {
                run(stop);
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
        while (!stop.stop_requested()) {
            const auto now = Clock::now();
            live.maintain(now);
            const auto update = live.poll();
            for (const auto& event : update.events)
                live.apply(event);
            // A feed that lost callbacks never recovers in place: exit, and
            // systemd starts a complete stream again.
            if (update.failed)
                throw std::runtime_error("market feed lost provider events");
            bool answered = false;
            while (auto call = calls.try_pop()) {
                (*call)(live);
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
        Call call([request = std::move(request)](const Live& live) {
            return reply(respond(request, [&](wire::Response& out) {
                answer_live(request, out, live);
            }));
        });
        auto result = call.get_future();
        if (!calls.try_push(std::move(call)))
            throw Error(ErrorCode::resource_exhausted, "market request queue is full or stopped");
        return waiting(std::move(result), id);
    }
};

QuotesService::QuotesService(Settings settings)
    : running_(std::make_unique<Running>(std::move(settings)))
{}

QuotesService::~QuotesService() = default;

QuotesService::Reply QuotesService::serve(std::string frame, bool known)
{
    wire::Request request;
    try {
        if (!request.ParseFromString(frame))
            throw std::invalid_argument("malformed Protobuf request");
        protocol::validate_message(request);
        if (request.call_case() == wire::Request::CALL_NOT_SET)
            throw std::invalid_argument("missing quotes operation");
        if (!known)
            throw Error(ErrorCode::permission_denied, "this client is not registered");
        if (request.call_case() == wire::Request::kStreamQuotes)
            return running_->open_stream(std::move(request));
        return running_->ask_market_thread(std::move(request));
    } catch (const std::exception& error) {
        wire::Response response;
        response.set_id(request.id());
        fail(response, error);
        protocol::log_rpc_result("quotes", request, response);
        return ready(reply(response));
    }
}

void QuotesService::drain()
{
    running_->draining = true;
}

void QuotesService::stop()
{
    running_->calls.close();
    running_->thread.request_stop();
}

std::shared_future<void> QuotesService::stopped() const
{
    return running_->stopped;
}

} // namespace asterion::quotes
