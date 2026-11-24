#include "application_impl.hpp"
#include <asterion/base/id.hpp>

namespace asterion::terminal {

Application::Impl::Settings Application::Impl::inspect_settings(const Settings& previous) const
{
    SettingsView next{
        trading_connections.snapshot(),
        trading_connections.fingerprint(),
        endpoint_settings(market_endpoint),
        endpoint_settings(warehouse_endpoint)
    };
    return next == *previous ? previous : std::make_shared<const SettingsView>(std::move(next));
}

json Application::Impl::endpoint_settings(const SavedEndpoint& saved) const
{
    try {
        return saved.get();
    } catch (const std::exception&) {
        // The unreadable file stays untouched and is reported for inspection.
        return {{"error", "unreadable"}};
    }
}

void Application::Impl::register_connection_commands()
{
    const auto saved_commands =
        [this](const std::string& name, SavedEndpoint& saved, const bool live) {
            command(
                name + ".endpoint.save",
                [this, &saved, live](const json& p) -> PolledTask<Response> {
                    fields(p, {"host", "port", "fingerprint"});
                    if (!p.at("port").is_number_unsigned()
                        || p.at("port").get<std::uint64_t>() > 65535)
                        throw std::invalid_argument("invalid service port");
                    co_await settings<void>([&] {
                        saved.save(
                            text(p, "host"),
                            p.at("port").get<unsigned>(),
                            text(p, "fingerprint"));
                    });
                    if (live)
                        watch.reset();
                    co_return snapshot();
                });
            command(
                name + ".endpoint.clear",
                [this, &saved, live](const json& p) -> PolledTask<Response> {
                    fields(p, {});
                    co_await settings<void>([&] {
                        saved.clear();
                    });
                    if (live)
                        watch.reset();
                    co_return snapshot();
                });
        };
    saved_commands("market", market_endpoint, true);
    saved_commands("warehouse", warehouse_endpoint, false);
    // One trading.proto Request to the account's saved service, relayed unchanged.
    command("trading.call", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"account", "request"});
        const auto id = text(p, "account");
        validate_id(id);
        const auto find_connection = [&]() -> const json* {
            for (const auto& item : settings_view->trading)
                if (item.is_object() && item.value("id", "") == id)
                    return &item;
            return nullptr;
        };
        const auto* saved = find_connection();
        if (!saved)
            throw Error(ErrorCode::not_found, "trading connection is unavailable");
        if (saved->contains("error"))
            throw std::invalid_argument("invalid trading connection storage");
        const TradingEndpoint endpoint{
            saved->at("host").get<std::string>(),
            saved->at("instance").get<std::string>(),
            saved->at("port").get<std::uint16_t>(),
            trading_connections.tls(saved->at("fingerprint").get<std::string>())
        };
        const auto revision = saved->at("revision").get<std::string>();
        auto reply = co_await PollFuture{call_trading(service_io, endpoint, p.at("request"))};
        const auto* current = find_connection();
        if (!current || current->contains("error") || current->at("revision") != revision)
            throw Error(
                ErrorCode::conflict,
                "trading connection changed; inspect the original host before retrying");
        co_return Response{json{{"trading", std::move(reply)}}};
    });
    command("trading.connections.save", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "host", "port", "instance", "fingerprint", "revision"});
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
