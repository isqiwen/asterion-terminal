#include "market_client.hpp"
#include "service_relay.hpp"
#include <asterion/market.pb.h>
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/system/ipc/rpc_client.hpp>
#include <asterion/system/trace.hpp>
#include <asterion/version.hpp>
#include <google/protobuf/util/json_util.h>
#include <atomic>
#include <deque>

namespace asterion::terminal {
namespace wire = ::market;
using namespace std::chrono_literals;

std::future<Json> call_market(ServiceIo& io, ServiceEndpoint endpoint, Json payload)
{
    return relay_call<wire::Request, wire::Response>(
        io,
        std::move(endpoint),
        std::move(payload),
        [](const wire::Request& request) {
            // A watch is a stream; the page reads it through market.watch.
            if (request.has_stream_quotes())
                throw std::invalid_argument("unsupported Market operation");
        },
        build::market_protocol);
}

struct MarketWatch::Impl
{
    Impl(ServiceIo& owner, ServiceEndpoint value)
        : io(owner),
          endpoint(std::move(value)),
          transport(relay_connect(io, endpoint))
    {}

    bool stopped(std::stop_token stop) const
    {
        return stop.stop_requested() || lifetime.stop_requested();
    }

    PolledTask<void> observe(std::stop_token stop)
    {
        while (!stopped(stop)) {
            try {
                wire::Request request;
                request.mutable_stream_quotes();
                relay_identify(request);
                std::optional<Payload> frame;
                auto watching =
                    transport->watch(request.SerializeAsString(), 2s, [&](Payload bytes) {
                        frame = std::move(bytes);
                    });
                // A new stream starts with a complete frame; older ones are not needed.
                frames.clear();
                while (watching.wait_for(0ms) != std::future_status::ready) {
                    if (stopped(stop))
                        transport->cancel();
                    else
                        transport->poll();
                    if (frame) {
                        auto raw = std::move(*frame);
                        frame.reset();
                        // RpcClient yields one frame per poll; apply it before polling again.
                        auto value = co_await io.read<Json>([raw = std::move(raw), &request] {
                            auto response = relay_decode<wire::Response>(*raw, request);
                            if (!response.has_stream_quotes())
                                throw Error(ErrorCode::unavailable, "missing quote frame");
                            return relay_json(response.stream_quotes());
                        });
                        frames.push_back(std::move(value));
                        online = true;
                        error.clear();
                        seen = std::chrono::steady_clock::now();
                        // About 16 s of frames: the page stopped reading, so start over.
                        if (frames.size() > 64) {
                            restarting = true;
                            transport->cancel();
                        }
                    }
                    co_await std::suspend_always{};
                }
                watching.get();
            } catch (const std::exception& failure) {
                transport->cancel();
                online = false;
                // Ending the stream here on purpose is not a failure to report.
                if (!restarting)
                    error = failure.what();
            }
            frames.clear();
            const auto next = std::chrono::steady_clock::now() + 2s;
            co_await PollUntil{[&] {
                return stopped(stop) || restarting || std::chrono::steady_clock::now() >= next;
            }};
            restarting = false;
        }
    }

    ServiceIo& io;
    const ServiceEndpoint endpoint;
    std::unique_ptr<ipc::RpcClient> transport;
    std::stop_source lifetime;
    std::deque<Json> frames;
    bool online = false, restarting = false;
    std::string error;
    std::chrono::steady_clock::time_point seen{};
};

MarketWatch::MarketWatch(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl))
{}

std::future<std::shared_ptr<MarketWatch>> MarketWatch::open(ServiceIo& io, ServiceEndpoint endpoint)
{
    return io.submit<std::shared_ptr<MarketWatch>>(
        [&io, endpoint = std::move(endpoint)](
            std::stop_token) mutable -> PolledTask<std::shared_ptr<MarketWatch>> {
            // A wrong address or pin is reported to the owner who asked to connect.
            Json heartbeat{{"getStatus", Json::object()}};
            (void)co_await PollFuture{call_market(io, endpoint, std::move(heartbeat))};
            auto impl = co_await io.admin<std::shared_ptr<Impl>>([&] {
                return std::make_shared<Impl>(io, std::move(endpoint));
            });
            (void)io.submit<void>(
                [impl](std::stop_token stop) {
                    return impl->observe(stop);
                },
                ServiceIo::Lane::observation);
            co_return std::shared_ptr<MarketWatch>(new MarketWatch(std::move(impl)));
        });
}

MarketWatch::~MarketWatch()
{
    impl_->lifetime.request_stop();
}

Json MarketWatch::take(bool restart)
{
    if (restart) {
        impl_->frames.clear();
        impl_->restarting = true;
        impl_->transport->cancel();
    }
    Json frames = Json::array();
    for (auto& frame : impl_->frames)
        frames.push_back(std::move(frame));
    impl_->frames.clear();
    return {
        {"frames", std::move(frames)},
        {"online", impl_->online && std::chrono::steady_clock::now() - impl_->seen < 5s},
        {"error", impl_->error}
    };
}

ServiceEndpoint MarketWatch::endpoint() const
{
    return impl_->endpoint;
}

} // namespace asterion::terminal
