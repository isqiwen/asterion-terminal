#pragma once
#include "service_endpoint.hpp"
#include "service_io.hpp"
#include <asterion/base/serialization.hpp>
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/system/ipc/rpc_client.hpp>
#include <google/protobuf/util/json_util.h>
#include <atomic>
#include <chrono>
#include <memory>

namespace asterion::terminal {

// What Quotes and History calls share: one generated Request as ProtoJSON, on its
// own pinned TLS connection, and its Response back as ProtoJSON.
template <class Request>
void relay_identify(Request& request)
{
    static std::atomic<std::uint64_t> next{0};
    request.set_id(++next);
}

template <class Response, class Request>
Response relay_decode(const std::string& raw, const Request& request)
{
    Response response;
    if (!response.ParseFromString(raw))
        throw Error(ErrorCode::unavailable, "invalid service response");
    protocol::validate_message(response);
    if (response.id() != request.id())
        throw Error(ErrorCode::unavailable, "service response identity mismatch");
    if (response.has_error())
        protocol::raise(response.error());
    return response;
}

inline Json relay_json(const google::protobuf::Message& message)
{
    std::string encoded;
    if (!google::protobuf::util::MessageToJsonString(message, &encoded).ok())
        throw Error(ErrorCode::internal_error, "cannot encode service response");
    return Json::parse(encoded);
}

inline std::unique_ptr<ipc::RpcClient> relay_connect(ServiceIo& io, const ServiceEndpoint& endpoint)
{
    if (endpoint.tls.empty())
        throw std::invalid_argument("the service requires pinned mutual TLS");
    return std::make_unique<ipc::RpcClient>(
        io.reactor(),
        endpoint.host,
        endpoint.port,
        endpoint.tls,
        1,
        io.payload_budget(ServiceIo::PayloadLane::data));
}

template <class Request, class Response, class Check>
std::future<Json> relay_call(ServiceIo& io, ServiceEndpoint endpoint, Json payload, Check check)
{
    using namespace std::chrono_literals;
    return io.submit<Json>(
        [&io, endpoint = std::move(endpoint), payload = std::move(payload), check](
            std::stop_token stop) -> PolledTask<Json> {
            auto request = co_await io.read<Request>([&] {
                Request request;
                if (!google::protobuf::util::JsonStringToMessage(payload.dump(), &request).ok())
                    throw std::invalid_argument("invalid service request");
                check(request);
                relay_identify(request);
                return request;
            });
            auto transport = co_await io.admin<std::unique_ptr<ipc::RpcClient>>([&] {
                return relay_connect(io, endpoint);
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
                return relay_json(relay_decode<Response>(*raw, request));
            });
        });
}

} // namespace asterion::terminal
