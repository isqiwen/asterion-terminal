#include "trading_client.hpp"
#include <atomic>
#include <google/protobuf/util/json_util.h>
#include <asterion/system/ipc/rpc_client.hpp>
#include <asterion/system/trace.hpp>
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/trading.hpp>

namespace asterion::terminal {

using namespace std::chrono_literals;
namespace wire = ::trading;

std::future<Json> call_trading(ServiceIo& io, TradingEndpoint endpoint, Json payload)
{
    return io.submit<Json>(
        [&io, endpoint = std::move(endpoint), payload = std::move(payload)](
            std::stop_token stop) -> PolledTask<Json> {
            auto request = co_await io.read<wire::Request>([&] {
                wire::Request request;
                if (!google::protobuf::util::JsonStringToMessage(payload.dump(), &request).ok())
                    throw std::invalid_argument("invalid trading request");
                protocol::validate_message(request);
                static std::atomic<std::uint64_t> next{0};
                request.set_id(++next);
                log_request(request.id());
                return request;
            });
            auto transport = co_await io.admin<std::unique_ptr<ipc::RpcClient>>([&] {
                if (endpoint.tls.empty())
                    throw std::invalid_argument("Trading requires pinned mutual TLS");
                return std::make_unique<ipc::RpcClient>(
                    io.reactor(),
                    endpoint.host,
                    endpoint.port,
                    endpoint.tls,
                    1,
                    io.payload_budget(ServiceIo::PayloadLane::administration),
                    4 * 1024 * 1024);
            });
            auto reply = transport->request(request.SerializeAsString(), 30s);
            while (reply.wait_for(0ms) != std::future_status::ready) {
                if (stop.stop_requested())
                    transport->cancel();
                else
                    transport->poll();
                co_await std::suspend_always{};
            }
            co_return co_await io.read<Json>([raw = reply.get(), &request] {
                wire::Response response;
                if (!response.ParseFromString(*raw))
                    throw Error(ErrorCode::unavailable, "invalid Trading response");
                protocol::validate_message(response);
                if (response.id() != request.id())
                    throw Error(ErrorCode::unavailable, "Trading response identity mismatch");
                if (response.has_error())
                    protocol::raise(response.error());
                std::string encoded;
                if (!google::protobuf::util::MessageToJsonString(response, &encoded).ok())
                    throw Error(ErrorCode::internal_error, "cannot encode trading response");
                return Json::parse(encoded);
            });
        });
}

} // namespace asterion::terminal
