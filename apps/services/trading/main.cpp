#include <asterion/system/log.hpp>
#include <asterion/system/native_plugin.hpp>
#include "live_session.hpp"
#include "settings.hpp"
#include "account_journal.hpp"
#include <CLI/CLI.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/rpc_host.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/protocol/health.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <asterion/system/trace.hpp>
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;

// One CTP account's instance of an installed trading service (settings.hpp),
// started and restarted by systemd as asterion-trading@<account>.
int main(int argc, char** argv)
{
    CLI::App app{"Asterion CTP account service (one execution owner per account)"};
    app.set_version_flag("--version", "Asterion Trading 0.1.0");
    std::string account_name;
    app.add_option("--account", account_name, "The account instance to run")->required();
    argv = app.ensure_utf8(argv);
    CLI11_PARSE(app, argc, argv);
    try {
        const auto installation = asterion::installation_directory();
        asterion::log::init(installation, "asterion_trading-" + account_name);
        const auto settings = asterion::trading::load_settings(installation, account_name);
        namespace wire = asterion::protocol::proto;
        const auto& session_id = settings.account;
        const auto& sdk = settings.ctp_library;
        const auto configured_account =
            asterion::protocol::encode_trading_account(settings.identity);
        const auto& journal = settings.journal;
        const auto& ownership_directory = settings.owners;
        asterion::service::install_stop_signals();
        // I/O owns initialization, publication and shutdown; the account owns its resources.
        std::unique_ptr<asterion::trading::LiveSession> account_owner;
        asterion::trading::LiveSession* account = nullptr;
        std::shared_future<void> initializing;
        std::future<void> login;
        bool recovering = false;
        asterion::Progress initialization, io_progress;
        const auto started = std::chrono::steady_clock::now();
        const auto instance = asterion::unique_process_id();
        const auto fill_health = [&](wire::Health& h) {
            h.set_instance_id(instance);
            h.set_version("0.1.0");
            h.set_uptime_ms(
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started)
                        .count()));
            auto* e = h.mutable_execution();
            *e->mutable_io() = asterion::protocol::encode_progress(io_progress.observe());
            *e->mutable_initialization() =
                asterion::protocol::encode_progress(initialization.observe());
            const auto current = account_owner.get();
            h.set_initialized(bool(account));
            h.set_recovery_required(current && current->recovery_required());
            if (current) {
                const auto observed = current->health();
                *e->mutable_state() = asterion::protocol::encode_progress(observed.state);
                *e->mutable_persistence() =
                    asterion::protocol::encode_progress(observed.persistence);
                *e->mutable_command() = asterion::protocol::encode_progress(observed.command);
                e->set_business_ready(observed.business_ready);
            }
        };

        struct Pending
        {
            wire::Request request;
            wire::Response response;
            asterion::trading::LiveSession* session = nullptr;
            std::shared_future<void> initialization;
            std::future<void> action;
            std::future<wire::LiveSnapshot> snapshot;
            bool needs_snapshot = false;

            void fail(const std::exception& error)
            {
                response.mutable_error()->set_code(
                    std::string(asterion::error_name(asterion::classify(error))));
                response.mutable_error()->set_message(error.what());
                needs_snapshot = false;
            }
        };

        auto serve = [&](const asterion::service::RpcHost::Peer&, std::string frame) {
            auto work = std::make_shared<Pending>();
            auto& request = work->request;
            auto& response = work->response;
            response.set_version(1);
            response.set_session_id(session_id);
            *response.mutable_account() = configured_account;
            work->session = account;
            try {
                if (!request.ParseFromString(frame))
                    throw std::invalid_argument("malformed Protobuf request");
                asterion::protocol::validate_message(request);
                response.set_correlation_id(request.correlation_id());
                asterion::validate_id(request.correlation_id());
                asterion::TraceScope trace(request.correlation_id());
                if (request.version() != 1 || request.session_id() != session_id)
                    throw std::invalid_argument("protocol version or session mismatch");
                if (request.has_command())
                    asterion::validate_id(request.account_id());
                else if (!request.account_id().empty())
                    throw std::invalid_argument(
                        "account identity is only valid for trading commands");
                if (request.has_command())
                    asterion::validate_id(request.policy_revision());
                else if (!request.policy_revision().empty())
                    throw std::invalid_argument(
                        "policy revision is only valid for trading commands");
                if (request.has_heartbeat()) {
                    fill_health(*response.mutable_health());
                } else if (request.has_shutdown()) {
                    throw std::invalid_argument("the trading service is stopped by systemd");
                } else if ((request.has_attach() || request.has_snapshot()) && !work->session
                    && account_owner) {
                    work->session = account_owner.get();
                    work->initialization = account_owner->initialized();
                    work->needs_snapshot = true;
                } else if (request.has_attach() && !work->session) {
                    response.mutable_uninitialized();
                } else {
                    if (request.has_live_create()) {
                        if (account_owner)
                            throw std::invalid_argument("session already initialized");
                        auto manifest = settings.identity;
                        manifest["type"] = "live_ctp";
                        manifest["policy"] =
                            asterion::protocol::decode_live_policy(request.live_create());
                        account_owner = std::make_unique<asterion::trading::LiveSession>(
                            journal,
                            sdk,
                            ownership_directory,
                            manifest);
                        initialization.begin();
                        work->session = account_owner.get();
                        initializing = account_owner->initialized();
                        work->initialization = initializing;
                    } else if (!work->session) {
                        throw std::invalid_argument("session is not initialized");
                    } else if (request.has_live_connect()) {
                        work->action =
                            work->session->connect(settings.password, settings.auth_code);
                    } else if (request.has_live_disconnect()) {
                        work->action = work->session->disconnect();
                    } else if (request.has_live_costs()) {
                        work->action = work->session->query_costs();
                    } else if (request.has_command()) {
                        work->action = work->session->execute(
                            request.account_id(),
                            request.policy_revision(),
                            asterion::trading::AccountRequest::from_wire(request.command()));

                    } else if (!request.has_snapshot() && !request.has_attach()) {
                        throw std::invalid_argument("missing live session operation");
                    }
                    work->needs_snapshot = true;
                }
            } catch (const std::exception& error) {
                work->fail(error);
            }
            std::fill(frame.begin(), frame.end(), '\0');
            return [&, work]() -> std::optional<std::string> {
                auto& request = work->request;
                auto& response = work->response;
                asterion::TraceScope trace(request.correlation_id());
                try {
                    if (work->initialization.valid()) {
                        if (work->initialization.wait_for(0ms) != std::future_status::ready)
                            return {};
                        work->initialization.get();
                        if (account != work->session)
                            return {}; // The I/O coordinator publishes initialization once.
                        work->initialization = {};
                    }
                    if (work->action.valid()) {
                        if (work->action.wait_for(0ms) != std::future_status::ready)
                            return {};
                        work->action.get();
                    }
                    if (work->needs_snapshot) {
                        if (!work->snapshot.valid())
                            work->snapshot = work->session->snapshot();
                        if (work->snapshot.wait_for(0ms) != std::future_status::ready)
                            return {};
                        *response.mutable_live() = work->snapshot.get();
                    }
                } catch (const std::exception& error) {
                    work->fail(error);
                }
                const auto& command = request.command();
                const std::string_view order = command.has_submit() ? command.submit().order_id()
                    : command.has_cancel()                          ? command.cancel().order_id()
                    : command.has_live_resolve() ? command.live_resolve().order_id()
                                                 : std::string_view{};
                asterion::protocol::log_rpc_result(
                    "trading",
                    request,
                    response,
                    request.has_snapshot() || request.has_heartbeat() || request.has_attach(),
                    {{"session_id", session_id},
                        {"account_id", request.account_id()},
                        {"request_id", command.request_id()},
                        {"order_id", order}});
                request.Clear();
                return response.SerializeAsString();
            };
        };
        using Host = asterion::service::RpcHost;
        Host::Options options;
        options.advance = [&](Host::Stage stage) {
            if (!account_owner)
                return true;
            if (stage != Host::Stage::running)
                account_owner->close_admission();
            if (stage == Host::Stage::stopping_resources) {
                account_owner->stop();
                return account_owner->stopped().wait_for(0ms) == std::future_status::ready;
            }
            if (initializing.valid() && initializing.wait_for(0ms) == std::future_status::ready) {
                try {
                    initializing.get();
                    account = account_owner.get();
                    if (stage == Host::Stage::running)
                        login = account->connect(settings.password, settings.auth_code);
                } catch (...) {
                    account_owner.reset();
                    if (recovering)
                        throw;
                    // Creation callers retain the original future and receive its failure.
                }
                initializing = {};
                initialization.finish();
            }
            if (login.valid() && login.wait_for(0ms) == std::future_status::ready) {
                try {
                    login.get();
                } catch (const std::exception& error) {
                    // A local refusal (such as a record that needs recovery). The SDK
                    // retries an unreachable front itself; a rejected password is never
                    // retried, so it cannot lock the account.
                    asterion::log::error("trading account login refused: {}", error.what());
                }
            }
            return false;
        };
        Host host(settings.transport, serve, std::move(options), io_progress);
        // Socket setup completes before starting owners that require an asynchronous stop.
        if (std::filesystem::exists(journal / "journal.sqlite")) {
            const auto recorded = asterion::trading::account_manifest(
                asterion::read_journal_header(
                    journal,
                    asterion::trading::AccountJournal::sidecars));
            for (const auto* key : {"account_id", "broker", "environment"})
                if (recorded.at(key) != settings.identity.at(key))
                    throw std::invalid_argument(
                        "trading account configuration differs from its journal: "
                        + (installation / "config/accounts" / (account_name + ".json")).string());
            initialization.begin();
            account_owner =
                std::make_unique<asterion::trading::LiveSession>(journal, sdk, ownership_directory);
            recovering = true;
            initializing = account_owner->initialized();
        }
        if (!host.run())
            std::_Exit(0);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Trading process failed: " << error.what() << '\n';
        asterion::log::error("trading service failed: {}", error.what());
        return 1;
    }
}
