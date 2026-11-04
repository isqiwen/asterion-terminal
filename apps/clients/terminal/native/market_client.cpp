#include "market_client.hpp"
#include <asterion/market.pb.h>
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/system/ipc/rpc_client.hpp>
#include <asterion/system/trace.hpp>
#include <google/protobuf/util/json_util.h>
#include <atomic>
#include <deque>

namespace asterion::terminal {
namespace wire = ::market;
using namespace std::chrono_literals;

namespace {

void identify(wire::Request& request)
{
    static std::atomic<std::uint64_t> next{0};
    request.set_id(++next);
}

wire::Response decode(const std::string& raw, const wire::Request& request)
{
    wire::Response response;
    if (!response.ParseFromString(raw))
        throw Error(ErrorCode::unavailable, "invalid market response");
    protocol::validate_message(response);
    if (response.id() != request.id())
        throw Error(ErrorCode::unavailable, "market response identity mismatch");
    if (response.has_error())
        protocol::raise(response.error());
    return response;
}

Json json_of(const google::protobuf::Message& message)
{
    std::string encoded;
    if (!google::protobuf::util::MessageToJsonString(message, &encoded).ok())
        throw Error(ErrorCode::internal_error, "cannot encode market response");
    return Json::parse(encoded);
}

std::unique_ptr<ipc::RpcClient> connect(ServiceIo& io, const ServiceEndpoint& endpoint)
{
    if (endpoint.tls.empty())
        throw std::invalid_argument("Market requires pinned mutual TLS");
    return std::make_unique<ipc::RpcClient>(
        io.reactor(),
        endpoint.host,
        endpoint.port,
        endpoint.tls,
        1,
        io.payload_budget(ServiceIo::PayloadLane::data));
}

} // namespace

std::future<Json> call_market(ServiceIo& io, ServiceEndpoint endpoint, Json payload)
{
    return io.submit<Json>(
        [&io, endpoint = std::move(endpoint), payload = std::move(payload)](
            std::stop_token stop) -> PolledTask<Json> {
            auto request = co_await io.read<wire::Request>([&] {
                wire::Request request;
                if (!google::protobuf::util::JsonStringToMessage(payload.dump(), &request).ok())
                    throw std::invalid_argument("invalid Market request");
                // A watch is a stream; the page reads it through market.watch.
                if (request.has_stream_quotes())
                    throw std::invalid_argument("unsupported Market operation");
                identify(request);
                return request;
            });
            auto transport = co_await io.admin<std::unique_ptr<ipc::RpcClient>>([&] {
                return connect(io, endpoint);
            });
            auto reply = transport->request(request.SerializeAsString(), 60s);
            while (reply.wait_for(0ms) != std::future_status::ready) {
                if (stop.stop_requested())
                    transport->cancel();
                else
                    transport->poll();
                co_await std::suspend_always{};
            }
            co_return co_await io.read<Json>([raw = reply.get(), &request] {
                return json_of(decode(*raw, request));
            });
        });
}

struct MarketWatch::Impl
{
    ServiceIo& io;
    const ServiceEndpoint endpoint;
    std::unique_ptr<ipc::RpcClient> transport;
    std::stop_source lifetime;
    std::deque<Json> frames;
    bool online = false, restarting = false;
    std::chrono::steady_clock::time_point seen{};

    Impl(ServiceIo& owner, ServiceEndpoint value)
        : io(owner),
          endpoint(std::move(value)),
          transport(connect(io, endpoint))
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
                identify(request);
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
                        // RpcClient yields one frame per poll; apply it before polling again.
                        auto value = co_await io.read<Json>([raw = std::move(*frame), &request] {
                            auto response = decode(*raw, request);
                            if (!response.has_stream_quotes())
                                throw Error(ErrorCode::unavailable, "missing quote frame");
                            return json_of(response.stream_quotes());
                        });
                        frame.reset();
                        frames.push_back(std::move(value));
                        online = true;
                        seen = std::chrono::steady_clock::now();
                        // About 16 s of frames: the page stopped reading, so start over.
                        if (frames.size() > 64)
                            transport->cancel();
                    }
                    co_await std::suspend_always{};
                }
                watching.get();
            } catch (const std::exception&) {
                transport->cancel();
                online = false;
            }
            frames.clear();
            const auto next = std::chrono::steady_clock::now() + (restarting ? 0s : 2s);
            restarting = false;
            co_await PollUntil{[&] {
                return stopped(stop) || std::chrono::steady_clock::now() >= next;
            }};
        }
    }
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
        {"online", impl_->online && std::chrono::steady_clock::now() - impl_->seen < 5s}
    };
}

ServiceEndpoint MarketWatch::endpoint() const
{
    return impl_->endpoint;
}

} // namespace asterion::terminal
