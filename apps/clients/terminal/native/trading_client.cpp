#include "trading_client.hpp"
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/protocol/health.hpp>
#include <asterion/protocol/trading.hpp>

namespace asterion::terminal {

using namespace std::chrono_literals;
namespace wire = protocol::proto;

struct TradingClient::Impl
{
    ServiceIo& io;
    const TradingEndpoint endpoint;
    std::unique_ptr<ipc::RpcClient> transport;
    std::stop_source lifetime;
    // One request at a time. Beside it one cancel, and beside
    // those one revoke: none of them waits here for a command in flight, and a
    // revoke does not wait for a cancel the account service has queued either.
    // That service runs a cancel of a recorded order while a submission waits
    // for its quote, and closes the permission to send when it admits a revoke.
    bool busy = false, cancelling = false, revoking = false, failed = false;
    std::shared_ptr<const Json> last_snapshot;
    Json health = nullptr;
    Json account = nullptr;
    std::int64_t last_heartbeat_ms = 0, latency_ms = 0;
    unsigned reconnects = 0, reconnect_attempts = 0;

    Impl(ServiceIo& owner, TradingEndpoint value)
        : io(owner),
          endpoint(std::move(value))
    {
        validate_id(endpoint.session);
        if (endpoint.tls.empty())
            throw std::invalid_argument("trading requires certificate fingerprints");
        transport = std::make_unique<ipc::RpcClient>(
            io.reactor(),
            endpoint.host,
            endpoint.port,
            endpoint.tls,
            3,
            io.payload_budget(ServiceIo::PayloadLane::control));
    }

    bool stopped(std::stop_token stop) const
    {
        return stop.stop_requested() || lifetime.stop_requested();
    }

    PolledTask<void> call(wire::Request request, std::stop_token stop)
    {
        const auto& command = request.command();
        auto& lane = command.has_live_revoke() ? revoking
            : command.has_cancel()             ? cancelling
                                               : busy;
        co_await PollUntil{[&] {
            return !lane || stopped(stop);
        }};
        if (stopped(stop))
            throw Error(ErrorCode::cancelled, "trading connection closed");
        if (failed)
            throw Error(
                ErrorCode::unavailable,
                "trading connection lost; reconnect this account to its saved trading service");
        lane = true;

        struct Release
        {
            bool& lane;

            ~Release()
            {
                lane = false;
            }
        } release{lane};

        request.set_version(1);
        request.set_session_id(endpoint.session);
        request.set_correlation_id(next_correlation_id());
        const bool mutation = request.has_command() || request.has_live_create()
            || request.has_live_connect() || request.has_live_disconnect()
            || request.has_live_costs();
        const auto sent = std::chrono::steady_clock::now();
        wire::Response response;
        try {
            auto reply = transport->request(request.SerializeAsString(), mutation ? 30s : 3s);
            while (reply.wait_for(0ms) != std::future_status::ready) {
                if (stopped(stop))
                    transport
                        .reset(); // Complete outstanding futures with an unknown-outcome failure.
                else
                    transport->poll();
                co_await std::suspend_always{};
            }
            auto parsed = co_await io.read<std::pair<wire::Response, std::shared_ptr<const Json>>>(
                [raw = reply.get(), previous = last_snapshot, &request] {
                    wire::Response value;
                    if (!value.ParseFromString(*raw))
                        throw Error(ErrorCode::unavailable, "invalid trading response");
                    protocol::validate_message(value);
                    if (value.version() != 1 || value.session_id() != request.session_id()
                        || value.correlation_id() != request.correlation_id())
                        throw Error(ErrorCode::unavailable, "trading response identity mismatch");
                    if (!value.has_error() && !value.has_live()
                        && !(request.has_attach() && value.has_uninitialized())
                        && !(request.has_heartbeat() && value.has_health()))
                        throw Error(ErrorCode::unavailable, "missing trading response");
                    std::shared_ptr<const Json> snapshot;
                    if (value.has_live()) {
                        auto next = protocol::decode_live_snapshot(value.live());
                        snapshot = previous && *previous == next
                            ? previous
                            : std::make_shared<const Json>(std::move(next));
                        value.clear_live();
                    }
                    return std::pair{std::move(value), std::move(snapshot)};
                },
                ServiceIo::ReadLane::response);
            response = std::move(parsed.first);
            if (response.has_account()) {
                auto observed = protocol::decode_trading_account(response.account());
                if (!account.is_null() && account != observed)
                    throw Error(
                        ErrorCode::conflict,
                        "trading service account configuration changed; reconnect explicitly");
                account = std::move(observed);
            }
            if (response.has_health()) {
                const auto& h = response.health();
                if (h.instance_id().empty() || h.version().empty() || !h.has_execution())
                    throw Error(ErrorCode::unavailable, "invalid service health");
                last_heartbeat_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                                        .count();
                latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - sent)
                                 .count();
                health = {
                    {"instance_id", h.instance_id()},
                    {"version", h.version()},
                    {"uptime_ms", h.uptime_ms()},
                    {"execution", protocol::execution_health_json(h.execution())},
                    {"phase", protocol::service_health_name(protocol::trading_health_phase(h))}
                };
            } else if (response.has_uninitialized()) {
                if (last_snapshot)
                    throw Error(ErrorCode::unavailable, "remote ledger is no longer initialized");
            } else if (parsed.second)
                last_snapshot = std::move(parsed.second);
        } catch (...) {
            failed = true;
            throw;
        }
        if (response.has_error())
            throw_remote_error(response.error().code(), response.error().message());
        co_return;
    }

    PolledTask<void> initialize(std::stop_token stop)
    {
        wire::Request attach;
        attach.mutable_attach();
        co_await call(std::move(attach), stop);
        if (account.is_null())
            throw Error(
                ErrorCode::unavailable,
                "trading service did not provide its account configuration");
        wire::Request ping;
        ping.mutable_heartbeat();
        co_await call(std::move(ping), stop);
    }

    PolledTask<void> monitor(std::stop_token stop)
    {
        auto next = std::chrono::steady_clock::now() + 500ms;
        auto health_due = std::chrono::steady_clock::now() + 5s;
        while (!stopped(stop)) {
            co_await PollUntil{[&] {
                return stopped(stop) || std::chrono::steady_clock::now() >= next;
            }};
            if (stopped(stop))
                break;
            if (failed && reconnect_attempts >= 3) {
                next = std::chrono::steady_clock::now() + 5s;
                continue;
            }
            try {
                const bool reconnecting = failed;
                if (reconnecting) {
                    ++reconnect_attempts;
                    ++reconnects;
                    failed = false;
                }
                wire::Request observe;
                observe.mutable_attach();
                co_await call(std::move(observe), stop);
                if (reconnecting || std::chrono::steady_clock::now() >= health_due) {
                    wire::Request ping;
                    ping.mutable_heartbeat();
                    co_await call(std::move(ping), stop);
                    health_due = std::chrono::steady_clock::now() + 5s;
                }
                reconnect_attempts = 0;
            } catch (const std::exception&) {
                failed = true;
            }
            next = std::chrono::steady_clock::now() + (failed ? 5s : 500ms);
        }
        transport.reset();
        co_return;
    }

    TradingClient::Read read() const
    {
        return {last_snapshot, connection(), account, failed};
    }

    Json connection() const
    {
        Json status{
            {"reconnects", reconnects},
            {"restarts", 0},
            {"health", health},
            {"last_heartbeat_ms", last_heartbeat_ms},
            {"latency_ms", latency_ms},
            {"session", endpoint.session},
            {"state", failed ? "disconnected" : "connected"},
            {"transport", "tcp_tls"}
        };
        status.update({{"host", endpoint.host}, {"port", endpoint.port}});
        return status;
    }
};

TradingClient::TradingClient(ServiceIo& io, TradingEndpoint endpoint)
    : io_(io),
      impl_(std::make_shared<Impl>(io, std::move(endpoint)))
{}

std::future<std::shared_ptr<TradingClient>>
TradingClient::open(ServiceIo& io, TradingEndpoint endpoint)
{
    return io.submit<std::shared_ptr<TradingClient>>(
        [&io, endpoint = std::move(endpoint)](
            std::stop_token stop) mutable -> PolledTask<std::shared_ptr<TradingClient>> {
            auto client = co_await io.admin<std::shared_ptr<TradingClient>>([&] {
                return std::shared_ptr<TradingClient>(new TradingClient(io, std::move(endpoint)));
            });
            co_await client->impl_->initialize(stop);
            (void)client->io_.submit<void>(
                [state = client->impl_](std::stop_token stop) {
                    return state->monitor(stop);
                },
                ServiceIo::Lane::observation);
            co_return client;
        });
}

std::future<void> TradingClient::create(const Json& policy)
{
    auto input = protocol::encode_live_policy(policy);
    return io_.submit<void>(
        [state = impl_, input = std::move(input), policy](
            std::stop_token stop) -> PolledTask<void> {
            if (state->last_snapshot) {
                const auto& existing = *state->last_snapshot;
                for (const auto* key : {"risk", "contracts", "max_price_deviation"})
                    if (existing.at(key) != policy.at(key))
                        throw Error(
                            ErrorCode::conflict,
                            "trading account already initialized with a different definition");
                co_return;
            }
            wire::Request request;
            *request.mutable_live_create() = input;
            co_await state->call(std::move(request), stop);
            wire::Request ping;
            ping.mutable_heartbeat();
            co_await state->call(std::move(ping), stop);
        });
}

TradingClient::~TradingClient()
{
    impl_->lifetime.request_stop();
}

std::future<void> TradingClient::connect_broker()
{
    wire::Request request;
    request.mutable_live_connect();
    return io_.submit<void>(
        [state = impl_, request = std::move(request)](std::stop_token stop) -> PolledTask<void> {
            co_await state->call(request, stop);
        });
}

std::future<void> TradingClient::query_costs()
{
    wire::Request request;
    request.mutable_live_costs();
    return io_.submit<void>(
        [state = impl_, request = std::move(request)](std::stop_token stop) -> PolledTask<void> {
            co_await state->call(request, stop);
        });
}

std::future<void> TradingClient::disconnect_broker()
{
    wire::Request request;
    request.mutable_live_disconnect();
    return io_.submit<void>(
        [state = impl_, request = std::move(request)](std::stop_token stop) -> PolledTask<void> {
            co_await state->call(request, stop);
        });
}

Json TradingClient::Read::snapshot() const
{
    if (!session)
        return nullptr;
    auto value = *session;
    if (failed) {
        value["storage_state"] = "recovery_required";
        value["connection_state"] = "disconnected";
    }
    return value;
}

Json TradingClient::Read::render() const
{
    return {{"session", snapshot()}, {"connection", connection}, {"account", account}};
}

TradingClient::Read TradingClient::owner_view() const
{
    return impl_->read();
}

std::future<Json> TradingClient::view() const
{
    return io_.submit<Json>([state = impl_](std::stop_token) -> PolledTask<Json> {
        co_return co_await state->io.read<Json>(
            [projection = state->read()] {
                return projection.render();
            },
            ServiceIo::ReadLane::response);
    });
}

TradingEndpoint TradingClient::endpoint() const
{
    return impl_->endpoint;
}

std::future<void> TradingClient::execute(const Json& command)
{
    wire::Request request;
    auto payload = command;
    request.set_account_id(payload.at("account_id").get<std::string>());
    payload.erase("account_id");
    request.set_policy_revision(payload.at("policy_revision").get<std::string>());
    payload.erase("policy_revision");
    *request.mutable_command() = protocol::encode_command(payload);
    return io_.submit<void>(
        [state = impl_, request = std::move(request)](std::stop_token stop) -> PolledTask<void> {
            co_await state->call(request, stop);
        });
}

std::future<Json> TradingClient::snapshot() const
{
    return io_.submit<Json>([state = impl_](std::stop_token) -> PolledTask<Json> {
        co_return co_await state->io.read<Json>(
            [projection = state->read()] {
                return projection.snapshot();
            },
            ServiceIo::ReadLane::response);
    });
}

} // namespace asterion::terminal
