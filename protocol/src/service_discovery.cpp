#include <asterion/protocol/service_discovery.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/trace.hpp>
#include <algorithm>

namespace asterion::protocol {

node::proto::Request service_discovery_request(std::string_view parent)
{
    node::proto::Request request;
    request.set_version(1);
    TraceScope trace(parent);
    request.set_correlation_id(next_correlation_id());
    request.mutable_status();
    return request;
}

node::proto::Status
decode_service_discovery(const node::proto::Request& request, const std::string& bytes)
{
    node::proto::Response response;
    if (!request.has_status() || !response.ParseFromString(bytes))
        throw std::invalid_argument("invalid service discovery response");
    validate_message(response);
    if (response.version() != 1 || response.version() != request.version()
        || response.correlation_id() != request.correlation_id())
        throw std::invalid_argument("service discovery response identity mismatch");
    if (response.has_error())
        throw_remote_error(response.error().code(), response.error().message());
    if (!response.has_status())
        throw std::invalid_argument("service discovery returned no status");
    return std::move(*response.mutable_status());
}

node::proto::Status service_discovery(
    const std::string& endpoint,
    std::chrono::milliseconds timeout,
    std::stop_token stop)
{
    using namespace std::chrono_literals;
    if (timeout <= 0ms)
        throw Error(ErrorCode::unavailable, "service discovery deadline expired");
    const auto until = std::chrono::steady_clock::now() + timeout;
    ipc::Reactor reactor;
    ipc::RpcClient client(reactor, endpoint, 1, PayloadBudget{4 * 1024 * 1024}, 4 * 1024 * 1024);
    const auto request = service_discovery_request(current_trace_id());
    auto pending = client.request(request.SerializeAsString(), timeout, until, timeout);
    while (pending.wait_for(0ms) != std::future_status::ready) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "service discovery cancelled");
        client.poll();
        if (pending.wait_for(0ms) == std::future_status::ready)
            break;
        const auto left =
            std::chrono::ceil<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
        if (left <= 0ms)
            throw Error(ErrorCode::unavailable, "service discovery timed out");
        reactor.wait(std::min(left, 100ms));
    }
    return decode_service_discovery(request, *pending.get());
}

} // namespace asterion::protocol
