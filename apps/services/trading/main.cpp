#include <asterion/version.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/crash.hpp>
#include "alert_webhook.hpp"
#include "ctp_catalog.hpp"
#include "live_session.hpp"
#include "settings.hpp"
#include "strategy_host.hpp"
#include "systemd_watchdog.hpp"
#include "account_journal.hpp"
#include <CLI/CLI.hpp>
#include <asterion/base/error.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/file_lock.hpp>
#include <asterion/system/rpc_host.hpp>
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <asterion/system/trace.hpp>
#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;

// One CTP account's instance of an installed trading service (settings.hpp),
// started and restarted by systemd as asterion-trading@<account>.
int main(int argc, char** argv)
{
    CLI::App app{"Asterion CTP account service (one execution owner per account)"};
    app.set_version_flag(
        "--version",
        std::string("Asterion Trading ") + asterion::build::description);
    std::string account_name;
    app.add_option("--account", account_name, "The account instance to run")->required();
    bool check_login = false;
    app.add_flag(
        "--check-login",
        check_login,
        "Verify CTP credentials with a single login attempt and exit");
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
        if (check_login) {
            const auto settings = asterion::trading::load_settings(installation, account_name);
            const auto& broker = settings.identity.at("broker");
            const auto flow = std::filesystem::temp_directory_path()
                / ("asterion-login-check-" + asterion::unique_process_id());

            struct Cleanup
            {
                ~Cleanup()
                {
                    std::error_code ignored;
                    std::filesystem::remove_all(path, ignored);
                }

                std::filesystem::path path;
            } cleanup{flow};

            try {
                const auto trading_day = asterion::ctp::verify_ctp_login(
                    settings.ctp_library,
                    flow,
                    {settings.login.front,
                        broker.at("broker_id").get<std::string>(),
                        broker.at("user_id").get<std::string>(),
                        settings.login.password,
                        settings.login.app_id,
                        settings.login.auth_code});
                std::cout << "CTP login verified (trading_day=" << trading_day << ")\n";
                return 0;
            } catch (const asterion::Error& error) {
                std::cerr << error.what() << '\n';
                return error.code() == asterion::ErrorCode::unavailable ? 2 : 1;
            }
        }
        asterion::log::init(installation, "asterion_trading-" + account_name);
        const auto settings = asterion::trading::load_settings(installation, account_name);
#if defined(__linux__) && !defined(ASTERION_SANITIZED)
        asterion::install_crash_logging(installation / "logs", "asterion_trading-" + account_name);
#endif
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
            h.set_protocol(asterion::build::trading_protocol);
            h.set_emergency_protocol(asterion::build::emergency_protocol);
            h.set_version(asterion::build::description);
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

        const auto alerts =
            std::make_shared<asterion::trading::AlertWebhook>(account_name, settings.alert_webhook);
        asterion::trading::StrategyHost strategies(
            installation,
            journal,
            settings.market,
            alerts,
            settings.strategy_memory_limit_mb);

        // The result of a call is the field of the same number in Response.
        const auto result = [](wire::Response& response, int call_case) {
            return response.GetReflection()->MutableMessage(
                &response,
                response.GetDescriptor()->FindFieldByNumber(call_case));
        };

        const auto copy_when_ready = [&](int call_case, auto future) {
            return [call_case, result, ready = future.share()](wire::Response& out) {
                if (ready.wait_for(0ms) != std::future_status::ready)
                    return false;
                result(out, call_case)->CopyFrom(ready.get());
                return true;
            };
        };

        const auto account_step = [&](int call_case,
                                      asterion::trading::LiveSession* session,
                                      std::shared_future<void> init = {},
                                      std::shared_future<void> act = {}) {
            std::shared_future<wire::Session> snap;
            return [&, call_case, session, init = std::move(init), act = std::move(act), snap](
                       wire::Response& out) mutable {
                if (init.valid()) {
                    if (init.wait_for(0ms) != std::future_status::ready)
                        return false;
                    init.get();
                    if (account != session)
                        return false; // The I/O coordinator publishes initialization once.
                    init = {};
                }
                if (act.valid()) {
                    if (act.wait_for(0ms) != std::future_status::ready)
                        return false;
                    act.get();
                    act = {};
                }
                wire::Account reply = configured_account;
                if (session) {
                    if (!snap.valid())
                        snap = session->snapshot().share();
                    if (snap.wait_for(0ms) != std::future_status::ready)
                        return false;
                    *reply.mutable_session() = snap.get();
                }
                result(out, call_case)->CopyFrom(reply);
                return true;
            };
        };

        const auto emergency_step = [&](int call_case,
                                        asterion::trading::LiveSession* session,
                                        std::future<wire::Strategies> strat,
                                        std::shared_future<void> act = {}) {
            std::shared_future<wire::Session> snap;
            std::optional<wire::Strategies> strategies_out;
            return [&,
                       call_case,
                       session,
                       strat_ready = strat.share(),
                       act = std::move(act),
                       snap,
                       strategies_out](wire::Response& out) mutable {
                if (act.valid()) {
                    if (act.wait_for(0ms) != std::future_status::ready)
                        return false;
                    act.get();
                    act = {};
                }
                if (!strategies_out) {
                    if (strat_ready.wait_for(0ms) != std::future_status::ready)
                        return false;
                    strategies_out = strat_ready.get();
                }
                ::emergency::State state;
                state.set_protocol(asterion::build::emergency_protocol);
                state.set_account_id(configured_account.id());
                state.set_account_open(bool(session));
                if (session) {
                    if (!snap.valid())
                        snap = session->snapshot().share();
                    if (snap.wait_for(0ms) != std::future_status::ready)
                        return false;
                    const auto& current = snap.get();
                    state.set_policy_revision(current.policy_revision());
                    state.set_broker_ready(current.broker_state() == wire::BROKER_STATE_READY);
                    state.set_authorized(current.has_authorized_ms());
                    for (const auto& o : current.working_orders()) {
                        auto* item = state.add_working_orders();
                        item->set_id(o.id());
                        item->set_exchange(o.instrument().exchange());
                        item->set_symbol(o.instrument().symbol());
                        item->set_side(o.side() == ::common::SIDE_BUY ? "buy" : "sell");
                        item->set_offset(
                            o.offset() == ::common::OFFSET_OPEN                  ? "open"
                                : o.offset() == ::common::OFFSET_CLOSE_TODAY     ? "close_today"
                                : o.offset() == ::common::OFFSET_CLOSE_YESTERDAY ? "close_yesterday"
                                                                                 : "close");
                        item->set_lots(o.lots().text());
                        item->set_price(o.price().text());
                    }
                }
                if (strategies_out->has_instance()) {
                    const auto& inst = strategies_out->instance();
                    if (inst.state() == wire::INSTANCE_STATE_STARTING
                        || inst.state() == wire::INSTANCE_STATE_WARMING
                        || inst.state() == wire::INSTANCE_STATE_READY)
                        state.set_active_strategy_id(inst.instance().id());
                }
                result(out, call_case)->CopyFrom(state);
                return true;
            };
        };

        struct Pending
        {
            void fail(const std::exception& error)
            {
                *response.mutable_error() = asterion::protocol::error(error);
                step = nullptr;
            }

            wire::Request request;
            wire::Response response;
            std::function<bool(wire::Response&)> step;
            // The Terminal logs the same id when it sends the request.
            std::string trace_id;
        };

        auto serve = [&](const asterion::service::RpcHost::Peer&, std::string frame) {
            auto work = std::make_shared<Pending>();
            auto& request = work->request;
            auto& response = work->response;
            auto& trace_id = work->trace_id;
            auto* session = account;
            try {
                if (!request.ParseFromString(frame))
                    throw std::invalid_argument("malformed Protobuf request");
                asterion::protocol::validate_message(request);
                response.set_id(request.id());
                trace_id = std::to_string(request.id());
                asterion::TraceScope trace(trace_id);
                const int call_case = request.call_case();
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
                        work->step =
                            copy_when_ready(call_case, strategies.request(request, account));
                        break;
                    case wire::Request::kGetRecordedBars:
                        work->step = copy_when_ready(
                            call_case,
                            strategies.recorded_bars(request.get_recorded_bars()));
                        break;
                    case wire::Request::kGetStrategyRun:
                        work->step = copy_when_ready(
                            call_case,
                            strategies.strategy_run(request.get_strategy_run()));
                        break;
                    case wire::Request::kGetCatalog: {
                        const auto& broker = settings.identity.at("broker");
                        work->step = copy_when_ready(
                            call_case,
                            strategies.catalog(
                                request.get_catalog(),
                                sdk,
                                {settings.login.front,
                                    broker.at("broker_id").get<std::string>(),
                                    broker.at("user_id").get<std::string>(),
                                    settings.login.password,
                                    settings.login.app_id,
                                    settings.login.auth_code}));
                        break;
                    }
                    case wire::Request::kGetAccount:
                        if (!session && account_owner)
                            work->step = account_step(
                                call_case,
                                account_owner.get(),
                                account_owner->initialized());
                        else
                            work->step = account_step(call_case, session);
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
                            manifest,
                            alerts);
                        initialization.begin();
                        initializing = account_owner->initialized();
                        work->step = account_step(call_case, account_owner.get(), initializing);
                        break;
                    }
                    case wire::Request::kGetDay: {
                        if (!session)
                            throw std::invalid_argument("the account is not open");
                        const auto& page = request.get_day();
                        const auto limit = page.limit() ? page.limit() : 50;
                        if (limit > 200)
                            throw std::invalid_argument("trade pages contain 1 to 200 records");
                        if (page.oldest_first() && page.trading_day().empty())
                            throw std::invalid_argument("a fill export requires its trading day");
                        work->step = copy_when_ready(
                            call_case,
                            session->day(
                                page.trading_day(),
                                page.offset(),
                                limit,
                                page.oldest_first()));
                        break;
                    }
                    case wire::Request::kGetOrders: {
                        if (!session)
                            throw std::invalid_argument("the account is not open");
                        const auto& page = request.get_orders();
                        const auto limit = page.limit() ? page.limit() : 50;
                        if (limit > 200)
                            throw std::invalid_argument("order pages contain 1 to 200 records");
                        work->step =
                            copy_when_ready(call_case, session->orders(page.offset(), limit));
                        break;
                    }
                    case wire::Request::kGetEmergency: {
                        wire::Request query;
                        query.mutable_get_strategies();
                        work->step = emergency_step(
                            call_case,
                            session,
                            strategies.request(std::move(query), account));
                        break;
                    }
                    case wire::Request::kEmergencyControl: {
                        const auto& ctl = request.emergency_control();
                        if (ctl.has_stop_strategy()) {
                            wire::Request stop_req;
                            stop_req.mutable_stop_strategy()->set_id(
                                ctl.stop_strategy().instance_id());
                            work->step = emergency_step(
                                call_case,
                                session,
                                strategies.request(std::move(stop_req), account));
                            break;
                        }
                        if (!ctl.has_revoke() && !ctl.has_cancel_order())
                            throw std::invalid_argument("missing emergency control operation");
                        if (!session)
                            throw std::invalid_argument("the account is not open");
                        wire::Command cmd;
                        if (ctl.has_revoke()) {
                            const auto& r = ctl.revoke();
                            asterion::validate_id(r.request_id());
                            asterion::validate_id(r.account_id());
                            asterion::validate_id(r.policy_revision());
                            cmd.set_request_id(r.request_id());
                            cmd.set_account_id(r.account_id());
                            cmd.set_policy_revision(r.policy_revision());
                            cmd.mutable_revoke();
                        } else {
                            const auto& c = ctl.cancel_order();
                            asterion::validate_id(c.request_id());
                            asterion::validate_id(c.account_id());
                            asterion::validate_id(c.policy_revision());
                            asterion::validate_id(c.order_id());
                            cmd.set_request_id(c.request_id());
                            cmd.set_account_id(c.account_id());
                            cmd.set_policy_revision(c.policy_revision());
                            cmd.mutable_cancel_order()->set_id(c.order_id());
                        }
                        auto act = session
                                       ->execute(
                                           cmd.account_id(),
                                           cmd.policy_revision(),
                                           asterion::trading::AccountRequest::from_wire(cmd))
                                       .share();
                        wire::Request query;
                        query.mutable_get_strategies();
                        work->step = emergency_step(
                            call_case,
                            session,
                            strategies.request(std::move(query), account),
                            std::move(act));
                        break;
                    }
                    default: {
                        if (!session)
                            throw std::invalid_argument("the account is not open");
                        std::future<void> act;
                        if (request.has_connect_broker())
                            act = session->connect(settings.login);
                        else if (request.has_disconnect_broker())
                            act = session->disconnect();
                        else if (request.has_query_costs())
                            act = session->query_costs();
                        else if (request.has_send_command()) {
                            const auto& command = request.send_command();
                            asterion::validate_id(command.account_id());
                            asterion::validate_id(command.policy_revision());
                            act = session->execute(
                                command.account_id(),
                                command.policy_revision(),
                                asterion::trading::AccountRequest::from_wire(command));
                        } else
                            throw std::invalid_argument("missing trading call");
                        work->step = account_step(call_case, session, {}, act.share());
                    }
                }
            } catch (const std::exception& error) {
                work->fail(error);
            }
            std::fill(frame.begin(), frame.end(), '\0');
            return [&, work]() -> std::optional<std::string> {
                auto& request = work->request;
                auto& response = work->response;
                asterion::TraceScope trace(work->trace_id);
                if (work->step) {
                    try {
                        if (!work->step(response))
                            return {};
                    } catch (const std::exception& error) {
                        work->fail(error);
                    }
                }
                const auto& command = request.send_command();
                const auto& emergency_ctl = request.emergency_control();
                const std::string_view account_id_meta = request.has_emergency_control()
                    ? (emergency_ctl.has_revoke() ? emergency_ctl.revoke().account_id()
                              : emergency_ctl.has_cancel_order()
                              ? emergency_ctl.cancel_order().account_id()
                              : std::string_view{})
                    : command.account_id();
                const std::string_view request_id_meta = request.has_emergency_control()
                    ? (emergency_ctl.has_revoke() ? emergency_ctl.revoke().request_id()
                              : emergency_ctl.has_cancel_order()
                              ? emergency_ctl.cancel_order().request_id()
                              : std::string_view{})
                    : command.request_id();
                const std::string_view order = request.has_emergency_control()
                    ? (emergency_ctl.has_cancel_order()
                              ? std::string_view{emergency_ctl.cancel_order().order_id()}
                              : std::string_view{})
                    : command.has_submit_order()  ? command.submit_order().order_id()
                    : command.has_cancel_order()  ? command.cancel_order().id()
                    : command.has_resolve_order() ? command.resolve_order().id()
                                                  : std::string_view{};
                asterion::protocol::log_rpc_result(
                    "trading",
                    request,
                    response,
                    request.has_get_account() || request.has_get_health()
                        || request.has_get_strategies() || request.has_get_day()
                        || request.has_get_orders() || request.has_get_catalog()
                        || request.has_get_recorded_bars() || request.has_get_strategy_run()
                        || request.has_get_emergency(),
                    {{"account_id", account_id_meta},
                        {"request_id", request_id_meta},
                        {"order_id", order}});
                request.Clear();
                return response.SerializeAsString();
            };
        };
        asterion::trading::SystemdWatchdog watchdog;
        using Host = asterion::service::RpcHost;
        Host::Options options;
        options.advance = [&](Host::Stage stage) {
            const auto health = account ? std::optional(account->health()) : std::nullopt;
            watchdog.notify_if_healthy(initialization.observe(), health ? &*health : nullptr);
            if (stage == Host::Stage::running)
                strategies.poll();
            if (stage == Host::Stage::stopping_resources) {
                alerts->close();
                strategies.stop();
            }
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
                        login = account->connect(settings.login);
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
                    alerts->notify(
                        "recovery_required",
                        "login_refused",
                        std::string("login refused: ") + error.what());
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
            // The front and AppID may change; another broker or investor is another account.
            const auto& broker = settings.identity.at("broker");
            if (recorded.at("account_id") != settings.identity.at("account_id")
                || recorded.at("environment") != settings.identity.at("environment")
                || recorded.at("broker").at("broker_id") != broker.at("broker_id")
                || recorded.at("broker").at("user_id") != broker.at("user_id"))
                throw std::invalid_argument(
                    "trading account configuration differs from its journal: "
                    + (installation / "config/accounts" / (account_name + ".json")).string());
            // A restart ends authorization, also when no strategy instance is interrupted.
            alerts->notify(
                "service_restarted",
                "service_restarted",
                "Trading restarted; authorize trading again");
            initialization.begin();
            account_owner = std::make_unique<asterion::trading::LiveSession>(
                journal,
                sdk,
                ownership_directory,
                nullptr,
                alerts);
            recovering = true;
            initializing = account_owner->initialized();
        }
        if (!host.run())
            std::_Exit(0);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Trading process failed: " << error.what() << '\n';
        if (!check_login)
            asterion::log::error("trading service failed: {}", error.what());
        return 1;
    }
}
