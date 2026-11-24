#include "application_impl.hpp"

namespace asterion::terminal {

std::optional<ServiceEndpoint>
Application::Impl::selected_of(const json& saved, const char* name) const
{
    if (saved.is_null())
        return std::nullopt;
    if (!saved.is_object() || saved.contains("error"))
        throw Error(ErrorCode::operation_failed, std::string(name) + " settings are unreadable");
    return ServiceEndpoint{
        saved.at("host").get<std::string>(),
        name,
        saved.at("port").get<std::uint16_t>(),
        {},
        trading_connections.tls(saved.at("fingerprint"))
    };
}

ServiceEndpoint Application::Impl::address_of(const json& saved, const char* name) const
{
    if (auto selected = selected_of(saved, name))
        return std::move(*selected);
    throw Error(ErrorCode::not_found, std::string("no ") + name + " is configured");
}

void Application::Impl::register_data_commands()
{
    // One market.proto Request to the saved service, relayed unchanged.
    command("market.call", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"request"});
        const auto address = address_of(settings_view->market_endpoint, "Market");
        auto reply = co_await PollFuture{call_market_json(service_io, address, p.at("request"))};
        // A settings edit meanwhile means the reply may be from another service.
        const auto current = selected_of(settings_view->market_endpoint, "Market");
        if (!current || !same_service_endpoint(*current, address))
            throw Error(
                ErrorCode::conflict,
                "service selection changed during operation; inspect the original service");
        co_return Response::raw("{\"market\":" + std::move(reply) + "}");
    });
    // One warehouse.proto Request to the saved service, relayed unchanged.
    command("warehouse.call", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"request"});
        const auto address = address_of(settings_view->warehouse_endpoint, "Warehouse");
        auto reply = co_await PollFuture{call_warehouse_json(service_io, address, p.at("request"))};
        const auto current = selected_of(settings_view->warehouse_endpoint, "Warehouse");
        if (!current || !same_service_endpoint(*current, address))
            throw Error(
                ErrorCode::conflict,
                "service selection changed during operation; inspect the original service");
        co_return Response::raw("{\"warehouse\":" + std::move(reply) + "}");
    });
    command("market.open", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        const auto address = address_of(settings_view->market_endpoint, "Market");
        auto next = co_await connect<std::shared_ptr<MarketWatch>>(
            [this, address]() -> PolledTask<std::shared_ptr<MarketWatch>> {
                co_return co_await PollFuture{MarketWatch::open(service_io, address)};
            });
        const auto current = selected_of(settings_view->market_endpoint, "Market");
        if (!current || !same_service_endpoint(*current, address))
            throw Error(ErrorCode::conflict, "Market address changed during connection");
        watch = std::move(next);
        co_return snapshot();
    });
    command("market.close", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        watch.reset();
        co_return snapshot();
    });
    // The frames of the open watch since the last call; null when none is open.
    command("market.watch", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"restart"});
        const bool restart = p.at("restart").get<bool>();
        co_return Response::raw(
            watch ? "{\"market_watch\":" + watch->take_json(restart) + "}"
                  : "{\"market_watch\":null}");
    });
    command("market.watchlist", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"instruments"});
        co_await settings<void>([&] {
            market_endpoint.select(p.at("instruments"));
        });
        co_return snapshot();
    });
}

} // namespace asterion::terminal
