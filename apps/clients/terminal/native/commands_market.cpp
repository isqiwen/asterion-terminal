#include "application_impl.hpp"

namespace asterion::terminal {

PolledTask<ServiceEndpoint> Application::Impl::market_address()
{
    const auto saved = co_await settings<json>([&] {
        return market_endpoint.get();
    });
    if (saved.is_null())
        throw std::invalid_argument("configure the remote Market address first");
    const auto tls = co_await settings<ipc::TlsIdentity>([&] {
        return trading_connections.tls(saved.at("fingerprint"));
    });
    co_return ServiceEndpoint{
        saved.at("host").get<std::string>(),
        "market-data",
        saved.at("port").get<std::uint16_t>(),
        {},
        "",
        "",
        tls
    };
}

void Application::Impl::register_market_commands()
{
    // One market.proto Request to the saved Market, relayed unchanged.
    command("market.call", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"request"});
        const auto address = co_await market_address();
        auto reply = co_await PollFuture{call_market(service_io, address, p.at("request"))};
        // A settings edit meanwhile means the reply may be from another Market.
        if (!same_service_endpoint(co_await market_address(), address))
            throw Error(
                ErrorCode::conflict,
                "market service selection changed during operation; inspect the original service before retrying");
        co_return Response{json{{"market", std::move(reply)}}};
    });
    command("market.open", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        const auto address = co_await market_address();
        auto next = co_await manage<std::shared_ptr<MarketWatch>>(
            [this, address]() -> PolledTask<std::shared_ptr<MarketWatch>> {
                co_return co_await PollFuture{MarketWatch::open(service_io, address)};
            });
        if (!same_service_endpoint(co_await market_address(), address))
            throw Error(ErrorCode::conflict, "market address changed during connection");
        market = std::move(next);
        co_return snapshot();
    });
    command("market.close", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        market.reset();
        co_return snapshot();
    });
    // The frames of the open watch since the last call; null when none is open.
    command("market.watch", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"restart"});
        const bool restart = p.at("restart").get<bool>();
        co_return Response{json{{"market_watch", market ? market->take(restart) : json(nullptr)}}};
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
