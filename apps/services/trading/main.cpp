#include <asterion/system/log.hpp>
#include "live_session.hpp"
#include "settings.hpp"
#include "strategy_host.hpp"
#include "account_journal.hpp"
#include <CLI/CLI.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/file_lock.hpp>
#include <asterion/system/rpc_host.hpp>
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/trading.hpp>
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
        const asterion::FileLock programs(
            installation,
            ".program.lock",
            asterion::FileLock::Access::shared_existing);
        if (std::filesystem::exists(installation / ".program-incomplete"))
            throw std::runtime_error(
                "Trading installation is incomplete; rerun its setup release: "
                + installation.string());
        asterion::log::init(installation, "asterion_trading-" + account_name);
        const auto settings = asterion::trading::load_settings(installation, account_name);
        namespace wire = ::trading;
        const auto& sdk = settings.ctp_library;
        const auto configured_account = asterion::protocol::encode_account(settings.identity);
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
        const auto fill_health = [&](wire::Health& h) {
            *h.mutable_io() = asterion::protocol::encode_progress(io_progress.observe());
            *h.mutable_initialization() =
                asterion::protocol::encode_progress(initialization.observe());
            const auto current = account_owner.get();
            h.set_account_open(bool(account));
            h.set_recovery_required(current && current->recovery_required());
            if (current) {
                const auto observed = current->health();
                *h.mutable_state() = asterion::protocol::encode_progress(observed.state);
                *h.mutable_persistence() =
                    asterion::protocol::encode_progress(observed.persistence);
                *h.mutable_command() = asterion::protocol::encode_progress(observed.command);
                h.set_ready(observed.business_ready);
            }
        };

        asterion::trading::StrategyHost strategies(installation, journal, settings.market);

        // The result of a call is the field of the same number in Response.
        const auto result = [](wire::Response& response, const wire::Request& request) {
            return response.GetReflection()->MutableMessage(
                &response,
                response.GetDescriptor()->FindFieldByNumber(request.call_case()));
        };

        struct Pending
        {
            wire::Request request;
            wire::Response response;
            asterion::trading::LiveSession* session = nullptr;
            std::shared_future<void> initialization;
            std::future<void> action;
            std::future<wire::Session> snapshot;
            std::future<wire::Strategies> strategy;
            bool needs_account = false;
            // The Terminal logs the same id when it sends the request.
            std::string trace_id;

            void fail(const std::exception& error)
            {
                *response.mutable_error() = asterion::protocol::error(error);
                needs_account = false;
            }
        };

        auto serve = [&](const asterion::service::RpcHost::Peer&, std::string frame) {
            auto work = std::make_shared<Pending>();
            auto& request = work->request;
            auto& response = work->response;
            auto& trace_id = work->trace_id;
            work->session = account;
            try {
                if (!request.ParseFromString(frame))
                    throw std::invalid_argument("malformed Protobuf request");
                asterion::protocol::validate_message(request);
                response.set_id(request.id());
                trace_id = std::to_string(request.id());
                asterion::TraceScope trace(trace_id);
                switch (request.call_case()) {
                    case wire::Request::kGetHealth:
                        fill_health(*response.mutable_get_health());
                        break;
                    case wire::Request::kGetStrategies:
                    case wire::Request::kPublishStrategy:
                    case wire::Request::kStartStrategy:
                    case wire::Request::kStopStrategy:
                    case wire::Request::kPauseStrategy:
                    case wire::Request::kFlattenStrategy:
                        work->strategy = strategies.request(request, account);
                        break;
                    case wire::Request::kGetAccount:
                        if (!work->session && account_owner) {
                            work->session = account_owner.get();
                            work->initialization = account_owner->initialized();
                        }
                        work->needs_account = true;
                        break;
                    case wire::Request::kOpenAccount: {
                        if (account_owner)
                            throw std::invalid_argument("the account is already open");
                        auto manifest = settings.identity;
                        manifest["type"] = "live_ctp";
                        manifest["policy"] =
                            asterion::protocol::decode_policy(request.open_account());
                        account_owner = std::make_unique<asterion::trading::LiveSession>(
                            journal,
                            sdk,
                            ownership_directory,
                            manifest);
                        initialization.begin();
                        work->session = account_owner.get();
                        initializing = account_owner->initialized();
                        work->initialization = initializing;
                        work->needs_account = true;
                        break;
                    }
                    default:
                        if (!work->session)
                            throw std::invalid_argument("the account is not open");
                        if (request.has_connect_broker())
                            work->action =
                                work->session->connect(settings.password, settings.auth_code);
                        else if (request.has_disconnect_broker())
                            work->action = work->session->disconnect();
                        else if (request.has_query_costs())
                            work->action = work->session->query_costs();
                        else if (request.has_send_command()) {
                            const auto& command = request.send_command();
                            asterion::validate_id(command.account_id());
                            asterion::validate_id(command.policy_revision());
                            work->action = work->session->execute(
                                command.account_id(),
                                command.policy_revision(),
                                asterion::trading::AccountRequest::from_wire(command));
                        } else
                            throw std::invalid_argument("missing trading call");
                        work->needs_account = true;
                }
            } catch (const std::exception& error) {
                work->fail(error);
            }
            std::fill(frame.begin(), frame.end(), '\0');
            return [&, work]() -> std::optional<std::string> {
                auto& request = work->request;
                auto& response = work->response;
                asterion::TraceScope trace(work->trace_id);
                try {
                    if (work->strategy.valid()) {
                        if (work->strategy.wait_for(0ms) != std::future_status::ready)
                            return {};
                        result(response, request)->CopyFrom(work->strategy.get());
                    }
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
                    if (work->needs_account) {
                        wire::Account reply = configured_account;
                        if (work->session) {
                            if (!work->snapshot.valid())
                                work->snapshot = work->session->snapshot();
                            if (work->snapshot.wait_for(0ms) != std::future_status::ready)
                                return {};
                            *reply.mutable_session() = work->snapshot.get();
                        }
                        result(response, request)->CopyFrom(reply);
                    }
                } catch (const std::exception& error) {
                    work->fail(error);
                }
                const auto& command = request.send_command();
                const std::string_view order = command.has_submit_order()
                    ? command.submit_order().order_id()
                    : command.has_cancel_order()  ? command.cancel_order().id()
                    : command.has_resolve_order() ? command.resolve_order().id()
                                                  : std::string_view{};
                asterion::protocol::log_rpc_result(
                    "trading",
                    request,
                    response,
                    request.has_get_account() || request.has_get_health()
                        || request.has_get_strategies(),
                    {{"account_id", command.account_id()},
                        {"request_id", command.request_id()},
                        {"order_id", order}});
                request.Clear();
                return response.SerializeAsString();
            };
        };
        using Host = asterion::service::RpcHost;
        Host::Options options;
        options.advance = [&](Host::Stage stage) {
            if (stage == Host::Stage::running)
                strategies.poll();
            if (stage == Host::Stage::stopping_resources)
                strategies.stop();
            if (!account_owner)
                return stage == Host::Stage::stopping_resources && strategies.stopped();
            if (stage != Host::Stage::running)
                account_owner->close_admission();
            if (stage == Host::Stage::stopping_resources) {
                if (!strategies.stopped())
                    return false;
                account_owner->stop();
                return account_owner->stopped().wait_for(0ms) == std::future_status::ready
                    && strategies.stopped();
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
