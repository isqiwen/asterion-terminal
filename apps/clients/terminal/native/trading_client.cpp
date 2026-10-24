#include "trading_client.hpp"
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/protocol/health.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/protocol/trading_facts.hpp>

namespace asterion::terminal {

using namespace std::chrono_literals;
namespace wire = protocol::proto;

struct TradingClient::Impl
{
    ServiceIo& io;
    const ServiceEndpoint endpoint;
    std::unique_ptr<ipc::RpcClient> transport;
    std::unique_ptr<ipc::RpcClient> facts_transport;
    bool facts_busy = false;
    std::stop_source lifetime;
    // One request at a time. Beside it one cancel or strategy stop, and beside
    // those one revoke: none of them waits here for a command in flight, and a
    // revoke does not wait for a cancel the account service has queued either.
    // That service runs a cancel of a recorded order while a submission waits
    // for its quote, and closes the permission to send when it admits a revoke.
    bool busy = false, cancelling = false, revoking = false, failed = false;
    std::shared_ptr<const Json> last_snapshot;
    Json health = nullptr;
    std::int64_t last_heartbeat_ms = 0, latency_ms = 0;
    unsigned reconnects = 0, reconnect_attempts = 0;

    Impl(ServiceIo& owner, ServiceEndpoint value)
        : io(owner),
          endpoint(std::move(value))
    {
        validate_id(endpoint.session);
        transport = endpoint.endpoint.empty()
            ? std::make_unique<ipc::RpcClient>(
                  io.reactor(),
                  endpoint.host,
                  endpoint.port,
                  endpoint.tls,
                  3,
                  io.payload_budget(ServiceIo::PayloadLane::control))
            : std::make_unique<ipc::RpcClient>(
                  io.reactor(),
                  endpoint.endpoint,
                  3,
                  io.payload_budget(ServiceIo::PayloadLane::control));
        auto budget = io.payload_budget(ServiceIo::PayloadLane::data);
        facts_transport = endpoint.endpoint.empty()
            ? std::make_unique<ipc::RpcClient>(
                  io.reactor(),
                  endpoint.host,
                  endpoint.port,
                  endpoint.tls,
                  2,
                  budget)
            : std::make_unique<ipc::RpcClient>(io.reactor(), endpoint.endpoint, 2, budget);
    }

    bool stopped(std::stop_token stop) const
    {
        return stop.stop_requested() || lifetime.stop_requested();
    }

    PolledTask<void> call(wire::Request request, std::stop_token stop)
    {
        const auto& command = request.command();
        auto& lane = command.has_live_revoke()                    ? revoking
            : command.has_cancel() || command.has_strategy_stop() ? cancelling
                                                                  : busy;
        co_await PollUntil{[&] {
            return !lane || stopped(stop);
        }};
        if (stopped(stop))
            throw Error(ErrorCode::cancelled, "trading connection closed");
        if (failed)
            throw Error(
                ErrorCode::unavailable,
                "trading connection lost; reconnect remote sessions in " "Settings, recover local sessions from their directory");
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
        const bool mutation = request.has_command() || request.has_recover()
            || request.has_live_create() || request.has_live_connect()
            || request.has_live_disconnect() || request.has_live_costs();
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

    PolledTask<Json> facts_call(wire::TradingFactsQuery query, std::stop_token stop)
    {
        if (endpoint.owner_node_id.empty())
            throw Error(
                ErrorCode::unavailable,
                "fixed trading facts require a stable owning node identity");
        if (query.has_read()) {
            protocol::validate_trading_facts_reference(query.read());
            if (query.read().owner().node_id() != endpoint.owner_node_id
                || query.read().owner().service_id() != endpoint.session)
                throw Error(ErrorCode::conflict, "trading facts owner mismatch");
        } else if (query.has_capture())
            validate_id(query.capture());
        else if (query.has_status())
            validate_id(query.status());
        else
            throw std::invalid_argument("missing trading facts operation");
        co_await PollUntil{[&] {
            return !facts_busy || stopped(stop);
        }};
        if (stopped(stop))
            throw Error(ErrorCode::cancelled, "trading facts connection closed");
        facts_busy = true;

        struct Release
        {
            bool& busy;

            ~Release()
            {
                busy = false;
            }
        } release{facts_busy};

        wire::Request request;
        request.set_version(1);
        request.set_session_id(endpoint.session);
        request.set_correlation_id(next_correlation_id());
        *request.mutable_facts() = std::move(query);
        auto reply = facts_transport->request(request.SerializeAsString(), 30s);
        while (reply.wait_for(0ms) != std::future_status::ready) {
            if (stopped(stop))
                facts_transport.reset();
            else
                facts_transport->poll();
            co_await std::suspend_always{};
        }
        const auto expected_account =
            last_snapshot ? last_snapshot->at("account_id").get<std::string>() : std::string{};
        co_return co_await io.read<Json>([raw = reply.get(),
                                             request = std::move(request),
                                             node = endpoint.owner_node_id,
                                             expected_account] {
            wire::Response response;
            if (!response.ParseFromString(*raw))
                throw Error(ErrorCode::unavailable, "invalid trading facts response");
            protocol::validate_message(response);
            if (response.version() != request.version()
                || response.session_id() != request.session_id()
                || response.correlation_id() != request.correlation_id())
                throw Error(ErrorCode::unavailable, "trading facts response identity mismatch");
            if (response.has_error())
                throw_remote_error(response.error().code(), response.error().message());
            if (!response.has_facts())
                throw Error(ErrorCode::unavailable, "missing trading facts response");
            const auto& status = response.facts();
            auto result = protocol::decode_trading_facts_status(status);
            const auto& reference = status.operation().reference();
            if (reference.owner().node_id() != node
                || reference.owner().service_id() != request.session_id())
                throw Error(ErrorCode::conflict, "trading facts owner mismatch");
            const auto& query = request.facts();
            if ((query.has_read()
                    && reference.SerializeAsString() != query.read().SerializeAsString())
                || (query.has_capture() && reference.id() != query.capture())
                || (query.has_status() && reference.id() != query.status()))
                throw Error(ErrorCode::conflict, "trading facts operation identity mismatch");
            if ((query.has_read() || query.has_capture()) && !status.has_capture())
                throw Error(ErrorCode::unavailable, "fixed trading facts payload is missing");
            if (status.has_capture() && !expected_account.empty()
                && status.capture().facts().account_id() != expected_account)
                throw Error(ErrorCode::conflict, "trading facts belong to another account");
            return result;
        });
    }

    PolledTask<void>
    initialize(Json manifest, const std::string& expected_account, std::stop_token stop)
    {
        wire::Request attach;
        attach.mutable_attach();
        co_await call(std::move(attach), stop);
        if (last_snapshot && !expected_account.empty()
            && last_snapshot->at("account_id") != expected_account)
            throw Error(ErrorCode::conflict, "trading service belongs to another account");
        if (!manifest.is_null()) {
            wire::Request create;
            *create.mutable_live_create() = protocol::encode_live_input(manifest);
            co_await call(std::move(create), stop);
        }
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
        facts_transport.reset();
        co_return;
    }

    TradingClient::Read read() const
    {
        return {last_snapshot, connection(), failed};
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
            {"transport", endpoint.endpoint.empty() ? "tcp_tls" : "local"}
        };
        if (endpoint.endpoint.empty())
            status.update({{"host", endpoint.host}, {"port", endpoint.port}});
        return status;
    }
};

TradingClient::TradingClient(ServiceIo& io, ServiceEndpoint endpoint)
    : io_(io),
      impl_(std::make_shared<Impl>(io, std::move(endpoint)))
{}

std::future<std::shared_ptr<TradingClient>> TradingClient::open(
    ServiceIo& io,
    ServiceEndpoint endpoint,
    Json manifest,
    std::string expected_account)
{
    return io.submit<std::shared_ptr<TradingClient>>(
        [&io,
            endpoint = std::move(endpoint),
            manifest = std::move(manifest),
            expected_account = std::move(expected_account)](
            std::stop_token stop) mutable -> PolledTask<std::shared_ptr<TradingClient>> {
            auto client = co_await io.admin<std::shared_ptr<TradingClient>>([&] {
                return std::shared_ptr<TradingClient>(new TradingClient(io, std::move(endpoint)));
            });
            co_await client->impl_->initialize(manifest, expected_account, stop);
            (void)client->io_.submit<void>(
                [state = client->impl_](std::stop_token stop) {
                    return state->monitor(stop);
                },
                ServiceIo::Lane::observation);
            co_return client;
        });
}

std::future<void> TradingClient::create(const Json& manifest)
{
    auto input = protocol::encode_live_input(manifest);
    return io_.submit<void>(
        [state = impl_, input = std::move(input), manifest](
            std::stop_token stop) -> PolledTask<void> {
            // Explicit continuation after a lost creation reply observes the same account.
            if (state->last_snapshot) {
                const auto& existing = *state->last_snapshot;
                if (existing.at("account_id") != manifest.at("account_id")
                    || existing.at("broker") != manifest.at("broker")
                    || existing.at("environment") != manifest.at("environment")
                    || existing.at("risk") != manifest.at("policy").at("risk")
                    || existing.at("contracts") != manifest.at("policy").at("contracts")
                    || existing.at("max_price_deviation")
                        != manifest.at("policy").at("max_price_deviation"))
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

std::future<Json> TradingClient::facts(wire::TradingFactsQuery query)
{
    return io_.submit<Json>(
        [state = impl_, query = std::move(query)](std::stop_token stop) mutable {
            return state->facts_call(std::move(query), stop);
        });
}

TradingClient::~TradingClient()
{
    impl_->lifetime.request_stop();
}

std::future<void> TradingClient::connect_broker(std::string password, std::string auth_code)
{
    wire::Request request;
    request.mutable_live_connect()->set_password(std::move(password));
    request.mutable_live_connect()->set_auth_code(std::move(auth_code));
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
    return {{"session", snapshot()}, {"connection", connection}};
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

ServiceEndpoint TradingClient::endpoint() const
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
