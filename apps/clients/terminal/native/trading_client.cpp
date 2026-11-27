#include "trading_client.hpp"
#include "service_relay.hpp"
#include <asterion/version.hpp>
#include <atomic>
#include <google/protobuf/util/json_util.h>
#include <asterion/system/ipc/rpc_client.hpp>
#include <asterion/system/trace.hpp>
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/trading.hpp>

namespace asterion::terminal {

using namespace std::chrono_literals;
namespace wire = ::trading;
constexpr std::size_t frame_bytes = 8 * 1024 * 1024;

namespace {

// Only non-urgent read-only Trading queries may reuse an idle keep-alive TLS stream.
bool read_only_trading(const wire::Request& request)
{
    switch (request.call_case()) {
        case wire::Request::kGetAccount:
        case wire::Request::kGetHealth:
        case wire::Request::kGetStrategies:
        case wire::Request::kGetDay:
        case wire::Request::kGetOrders:
        case wire::Request::kGetCatalog:
        case wire::Request::kQueryCosts:
            return true;
        default:
            return false;
    }
}

} // namespace

std::future<std::string> call_trading_json(ServiceIo& io, TradingEndpoint endpoint, Json payload)
{
    const bool urgent = urgent_trading_request(payload);
    const auto read_lane = urgent ? ServiceIo::ReadLane::response : ServiceIo::ReadLane::data;
    return io.submit<std::string>(
        [&io, endpoint = std::move(endpoint), payload = std::move(payload), read_lane](
            std::stop_token stop) -> PolledTask<std::string> {
            auto request = co_await io.read<wire::Request>(
                [&] {
                    wire::Request request;
                    if (!google::protobuf::util::JsonStringToMessage(payload.dump(), &request).ok())
                        throw std::invalid_argument("invalid trading request");
                    protocol::validate_message(request);
                    static std::atomic<std::uint64_t> next{0};
                    request.set_id(++next);
                    log_request(request.id());
                    return request;
                },
                read_lane);
            const bool keep_alive = read_only_trading(request);
            std::shared_ptr<ipc::RpcClient> transport;
            if (keep_alive)
                transport = co_await relay_shared_client(
                    io,
                    endpoint.host,
                    endpoint.port,
                    endpoint.tls,
                    ServiceIo::PayloadLane::administration,
                    frame_bytes);
            else
                transport = co_await io.admin<std::shared_ptr<ipc::RpcClient>>([&] {
                    if (endpoint.tls.empty())
                        throw std::invalid_argument("Trading requires pinned mutual TLS");
                    return std::make_shared<ipc::RpcClient>(
                        io.reactor(),
                        endpoint.host,
                        endpoint.port,
                        endpoint.tls,
                        1,
                        io.payload_budget(ServiceIo::PayloadLane::administration),
                        frame_bytes);
                });
            // Cold CTP catalog queries step through ~80 futures products at ~1.1 s each.
            const auto reply_timeout =
                request.call_case() == wire::Request::kGetCatalog ? 240s : 30s;
            auto reply = transport->request(
                request.SerializeAsString(),
                reply_timeout,
                std::chrono::steady_clock::time_point::max(),
                5s,
                keep_alive);
            while (reply.wait_for(0ms) != std::future_status::ready) {
                if (stop.stop_requested())
                    transport->cancel();
                else
                    transport->poll();
                co_await std::suspend_always{};
            }
            co_return co_await io.read<std::string>(
                [raw = reply.get(), &request] {
                    wire::Response response;
                    if (!response.ParseFromString(*raw))
                        throw Error(ErrorCode::unavailable, "invalid Trading response");
                    protocol::validate_message(response);
                    if (response.id() != request.id())
                        throw Error(ErrorCode::unavailable, "Trading response identity mismatch");
                    if (response.has_get_health()
                        && response.get_health().protocol() != build::trading_protocol)
                        throw Error(
                            ErrorCode::conflict,
                            std::string("trading") + service_protocol_differs);
                    if ((response.has_get_emergency()
                            && response.get_emergency().protocol() != build::emergency_protocol)
                        || (response.has_emergency_control()
                            && response.emergency_control().protocol()
                                != build::emergency_protocol))
                        throw Error(
                            ErrorCode::conflict,
                            std::string("trading") + service_protocol_differs);
                    if (response.has_error())
                        protocol::raise(response.error());
                    return relay_json_string(response);
                },
                read_lane);
        },
        urgent ? ServiceIo::Lane::administration : ServiceIo::Lane::request);
}

std::future<Json> call_trading(ServiceIo& io, TradingEndpoint endpoint, Json payload)
{
    const bool urgent = urgent_trading_request(payload);
    const auto read_lane = urgent ? ServiceIo::ReadLane::response : ServiceIo::ReadLane::data;
    return io.submit<Json>(
        [&io, endpoint = std::move(endpoint), payload = std::move(payload), read_lane](
            std::stop_token) -> PolledTask<Json> {
            auto encoded =
                co_await PollFuture{call_trading_json(io, std::move(endpoint), std::move(payload))};
            co_return co_await io.read<Json>(
                [encoded = std::move(encoded)] {
                    return Json::parse(encoded);
                },
                read_lane);
        },
        urgent ? ServiceIo::Lane::administration : ServiceIo::Lane::request);
}

} // namespace asterion::terminal
