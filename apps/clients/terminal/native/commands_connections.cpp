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
    // One trading.proto Request to the account's saved service, relayed unchanged.
    command("trading.call", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"account", "request"});
        const auto id = text(p, "account");
        const auto selected = co_await settings<std::pair<TradingEndpoint, std::string>>([&] {
            trading_connections.prepare_identity();
            return std::pair{
                trading_connections.endpoint(id),
                trading_connections.get(id).at("revision").get<std::string>()
            };
        });
        auto reply = co_await PollFuture{call_trading(service_io, selected.first, p.at("request"))};
        co_await settings<void>([&] {
            if (trading_connections.get(id).at("revision") != selected.second)
                throw Error(
                    ErrorCode::conflict,
                    "trading connection changed; inspect the original host before retrying");
        });
        co_return Response{json{{"trading", std::move(reply)}}};
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
        co_await settings<void>([&] {
            trading_connections.save(p, text(p, "revision", true));
        });
        co_return snapshot();
    });
    command("trading.connections.remove", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "revision"});
        co_await settings<void>([&] {
            trading_connections.remove(text(p, "id"), text(p, "revision"));
        });
        co_return snapshot();
    });
}

} // namespace asterion::terminal
