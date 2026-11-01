#include "application_impl.hpp"

namespace asterion::terminal {

Application::Impl::Settings Application::Impl::inspect_settings(const Settings& previous) const
{
    SettingsView next{
        trading_connections.snapshot(),
        trading_connections.fingerprint(),
        market_settings(),
        research_settings()
    };
    return next == *previous ? previous : std::make_shared<const SettingsView>(std::move(next));
}

json Application::Impl::market_settings() const
{
    try {
        return market_endpoint.get();
    } catch (const std::exception&) {
        // The unreadable file stays untouched and is reported for inspection.
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
    command("market.endpoint.save", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"host", "port", "fingerprint"});
        if (!p.at("port").is_number_unsigned() || p.at("port").get<std::uint64_t>() > 65535)
            throw std::invalid_argument("invalid market port");
        co_await settings<void>([&] {
            market_endpoint.save(
                text(p, "host"),
                p.at("port").get<unsigned>(),
                text(p, "fingerprint"));
        });
        market.reset();
        co_return snapshot();
    });
    command("market.endpoint.clear", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        co_await settings<void>([&] {
            market_endpoint.clear();
        });
        market.reset();
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
}

} // namespace asterion::terminal
