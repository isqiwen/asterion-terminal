#include "application_impl.hpp"

namespace asterion::terminal {

PolledTask<CtpConnection> Application::Impl::market_ctp()
{
    auto connection = (co_await settings<std::optional<CtpConnection>>([&] {
        return ctp_connections.market();
    }));
    if (!connection)
        throw std::invalid_argument("choose the CTP account for market data first");
    co_return std::move(*connection);
}

json Application::Impl::ctp_accounts() const
{
    return ctp_connections.snapshot();
}

Application::Impl::Settings Application::Impl::inspect_settings(const Settings& previous) const
{
    const auto selected = ctp_connections.market();
    SettingsView next{
        data_credentials.snapshot(),
        ctp_accounts(),
        selected ? json(selected->id) : json(nullptr),
        trading_connections.snapshot(),
        trading_connections.fingerprint(),
        recorder_settings(),
        research_settings()
    };
    return next == *previous ? previous : std::make_shared<const SettingsView>(std::move(next));
}

json Application::Impl::recorder_settings() const
{
    try {
        return recorder_endpoint.get();
    } catch (const std::exception&) {
        // Shown so it can be saved again; the file itself is left for inspection.
        return {{"error", "unreadable"}};
    }
}

json Application::Impl::research_settings() const
{
    try {
        return strategy_repository.get();
    } catch (const std::exception&) {
        return {{"error", "unreadable"}};
    }
}

void Application::Impl::register_connection_commands()
{
    command("research.repository.save", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"path"});
        co_await settings<void>([&] {
            strategy_repository.save(text(p, "path"));
        });
        co_return snapshot();
    });
    command("research.repository.clear", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        co_await settings<void>([&] {
            strategy_repository.clear();
        });
        co_return snapshot();
    });
    command("recorder.endpoint.save", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"host", "port"});
        if (!p.at("port").is_number_unsigned())
            throw std::invalid_argument("invalid recorder port");
        co_await settings<void>([&] {
            recorder_endpoint.save(text(p, "host"), p.at("port").get<unsigned>());
        });
        co_return snapshot();
    });
    command("recorder.endpoint.clear", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        co_await settings<void>([&] {
            recorder_endpoint.clear();
        });
        co_return snapshot();
    });
    command("trading.identity.prepare", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        co_await settings<void>([&] {
            trading_connections.prepare_identity();
        });
        co_return snapshot();
    });
    command("trading.connections.save", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "name", "host", "port", "instance", "fingerprint", "revision"});
        if (live.contains(text(p, "id")))
            throw Error(
                ErrorCode::conflict,
                "close the trading connection before editing or removing it");
        co_await settings<void>([&] {
            trading_connections.save(p, text(p, "revision", true));
        });
        co_return snapshot();
    });
    command("trading.connections.remove", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "revision"});
        if (live.contains(text(p, "id")))
            throw Error(
                ErrorCode::conflict,
                "close the trading connection before editing or removing it");
        co_await settings<void>([&] {
            trading_connections.remove(text(p, "id"), text(p, "revision"));
        });
        co_return snapshot();
    });
    // The lab's sources that need a credential, and its bounds: Tushare's token
    // and the requests per minute the lab sends at most.
    command("data.credentials.save", [this](const json& params) -> PolledTask<Response> {
        fields(params, {"provider", "credential", "remember", "requests_per_minute"});
        if (text(params, "provider") != "asterion.data.tushare")
            throw std::invalid_argument("this data source needs no credential");
        if (!params.at("requests_per_minute").is_number_unsigned()
            || !params.at("remember").is_boolean())
            throw std::invalid_argument("invalid data source credential settings");
        const DataCredentialLimits limits{true, 256, true, 500};
        (co_await settings<void>([&] {
            return data_credentials.save(
                {text(params, "provider"),
                    params.at("requests_per_minute").get<unsigned>(),
                    params.at("remember").get<bool>(),
                    text(params, "credential", true)},
                limits);
        }));
        co_return snapshot();
    });
    command("data.credentials.clear", [this](const json& params) -> PolledTask<Response> {
        fields(params, {"provider"});
        (co_await settings<void>([&] {
            return data_credentials.clear(text(params, "provider"));
        }));
        co_return snapshot();
    });
    command("ctp.connections.save", [this](const json& params) -> PolledTask<Response> {
        fields(
            params,
            {"id",
                "name",
                "revision",
                "broker_id",
                "user_id",
                "app_id",
                "trade_front",
                "market_front"});
        co_await settings<void>([&] {
            ctp_connections.save(
                {text(params, "id"),
                    text(params, "name"),
                    text(params, "broker_id"),
                    text(params, "user_id"),
                    text(params, "app_id"),
                    text(params, "trade_front"),
                    text(params, "market_front"),
                    {}},
                text(params, "revision", true));
        });
        co_return snapshot();
    });
    // Market data follows one account; it changes only while market data is
    // disconnected. Trading accounts are not affected.
    command("ctp.connections.market", [this](const json& params) -> PolledTask<Response> {
        fields(params, {"id"});
        if (market) {
            const auto phase = market->owner_read().header.at("phase").get<std::string>();
            if (phase != "disconnected" && phase != "error" && phase != "sdk_unavailable")
                throw Error(
                    ErrorCode::conflict,
                    "disconnect market data before changing its CTP account");
        }
        (co_await settings<void>([&] {
            return ctp_connections.select_market(text(params, "id"));
        }));
        co_return snapshot();
    });
    command("ctp.connections.remove", [this](const json& params) -> PolledTask<Response> {
        fields(params, {"id", "revision"});
        (co_await settings<void>([&] {
            return ctp_connections.remove(text(params, "id"), text(params, "revision"));
        }));
        co_return snapshot();
    });
}

} // namespace asterion::terminal
