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

// What Market and Warehouse calls share: one generated Request as ProtoJSON, on its
// own pinned TLS connection, and its Response back as ProtoJSON.
// After the service's protocol package: market, warehouse or trading.
inline constexpr char service_protocol_differs[] = " protocol differs from this Terminal";

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

inline std::string relay_json_string(const google::protobuf::Message& message)
{
    std::string encoded;
    if (!google::protobuf::util::MessageToJsonString(message, &encoded).ok())
        throw Error(ErrorCode::internal_error, "cannot encode service response");
    return encoded;
}

inline Json relay_json(const google::protobuf::Message& message)
{
    return Json::parse(relay_json_string(message));
}

inline std::string relay_shared_key(
    const std::string& host,
    std::uint16_t port,
    const ipc::TlsIdentity& tls,
    ServiceIo::PayloadLane lane,
    std::size_t frame_bytes)
{
    std::string key = host + ":" + std::to_string(port) + "|"
        + std::to_string(static_cast<int>(lane)) + "|" + std::to_string(frame_bytes) + "|"
        + tls.certificate_file + "|" + tls.private_key_file;
    for (const auto& peer : tls.peers)
        key += "|" + peer;
    return key;
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

inline PolledTask<std::shared_ptr<ipc::RpcClient>> relay_shared_client(
    ServiceIo& io,
    const std::string& host,
    std::uint16_t port,
    const ipc::TlsIdentity& tls,
    ServiceIo::PayloadLane lane,
    std::size_t frame_bytes = ipc::Channel::max_frame)
{
    if (tls.empty())
        throw std::invalid_argument("the service requires pinned mutual TLS");
    const auto key = relay_shared_key(host, port, tls, lane, frame_bytes);
    if (auto existing = io.shared_client(key))
        co_return existing;
    auto created = co_await io.admin<std::shared_ptr<ipc::RpcClient>>([&] {
        return std::make_shared<ipc::RpcClient>(
            io.reactor(),
            host,
            port,
            tls,
            32,
            io.payload_budget(lane),
            frame_bytes);
    });
    if (auto existing = io.shared_client(key))
        co_return existing;
    io.store_shared_client(key, created);
    co_return created;
}

template <class Request, class Response, class Check, class ReadOnly>
std::future<std::string> relay_call_json(
    ServiceIo& io,
    ServiceEndpoint endpoint,
    Json payload,
    Check check,
    ReadOnly read_only,
    std::string protocol)
{
    using namespace std::chrono_literals;
    return io.submit<std::string>(
        [&io,
            endpoint = std::move(endpoint),
            payload = std::move(payload),
            check,
            read_only,
            protocol = std::move(protocol)](std::stop_token stop) -> PolledTask<std::string> {
            auto request = co_await io.read<Request>([&] {
                Request request;
                if (!google::protobuf::util::JsonStringToMessage(payload.dump(), &request).ok())
                    throw std::invalid_argument("invalid service request");
                check(request);
                relay_identify(request);
                return request;
            });
            const bool keep_alive = read_only(request);
            std::shared_ptr<ipc::RpcClient> transport;
            if (keep_alive)
                transport = co_await relay_shared_client(
                    io,
                    endpoint.host,
                    endpoint.port,
                    endpoint.tls,
                    ServiceIo::PayloadLane::data);
            else
                transport = co_await io.admin<std::shared_ptr<ipc::RpcClient>>([&] {
                    return std::shared_ptr<ipc::RpcClient>(relay_connect(io, endpoint));
                });
            auto reply = transport->request(
                request.SerializeAsString(),
                60s,
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
            co_return co_await io.read<std::string>([raw = reply.get(), &request, &protocol] {
                auto response = relay_decode<Response>(*raw, request);
                if (response.has_get_status() && response.get_status().protocol() != protocol)
                    throw Error(
                        ErrorCode::conflict,
                        std::string(Response::descriptor()->file()->package())
                            + service_protocol_differs);
                return relay_json_string(response);
            });
        });
}

template <class Request, class Response, class Check>
std::future<Json>
relay_call(ServiceIo& io, ServiceEndpoint endpoint, Json payload, Check check, std::string protocol)
{
    return io.submit<Json>(
        [&io,
            endpoint = std::move(endpoint),
            payload = std::move(payload),
            check,
            protocol = std::move(protocol)](std::stop_token) -> PolledTask<Json> {
            auto encoded = co_await PollFuture{relay_call_json<Request, Response>(
                io,
                std::move(endpoint),
                std::move(payload),
                check,
                [](const Request&) {
                    return false;
                },
                std::move(protocol))};
            co_return co_await io.read<Json>([encoded = std::move(encoded)] {
                return Json::parse(encoded);
            });
        });
}

} // namespace asterion::terminal
