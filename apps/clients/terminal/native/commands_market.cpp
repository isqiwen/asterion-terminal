#include "application_impl.hpp"

namespace asterion::terminal {

void Application::Impl::register_market_commands()
{
    command("market.data", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"request"});
        const auto saved = co_await settings<json>([&] {
            return market_endpoint.get();
        });
        if (saved.is_null())
            throw std::invalid_argument("configure the remote Market address first");
        const auto tls = co_await settings<ipc::TlsIdentity>([&] {
            return trading_connections.tls(saved.at("fingerprint"));
        });
        const ServiceEndpoint address{
            saved.at("host").get<std::string>(),
            "market-data",
            saved.at("port").get<std::uint16_t>(),
            {},
            "",
            "",
            tls
        };
        auto reply = co_await PollFuture{MarketClient::data(service_io, address, p.at("request"))};
        const auto current = co_await settings<json>([&] {
            return market_endpoint.get();
        });
        if (current.is_null() || current.at("host") != saved.at("host")
            || current.at("port") != saved.at("port")
            || current.at("fingerprint") != saved.at("fingerprint"))
            throw Error(
                ErrorCode::conflict,
                "market service selection changed during operation; inspect the original service before retrying");
        auto result = read_published(json::object());
        result.values["market_data"] = std::move(reply);
        co_return result;
    });
    command("market.open", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        const auto saved = co_await settings<json>([&] {
            return market_endpoint.get();
        });
        if (saved.is_null())
            throw std::invalid_argument("configure the remote Market address first");
        const auto tls = co_await settings<ipc::TlsIdentity>([&] {
            return trading_connections.tls(saved.at("fingerprint"));
        });
        const ServiceEndpoint address{
            saved.at("host").get<std::string>(),
            "market-data",
            saved.at("port").get<std::uint16_t>(),
            {},
            "",
            "",
            tls
        };
        auto next = co_await manage<std::shared_ptr<MarketClient>>(
            [this, address]() -> PolledTask<std::shared_ptr<MarketClient>> {
                co_return co_await PollFuture{MarketClient::open(service_io, address)};
            });
        // A settings edit while the connection opens invalidates this selection.
        const auto current = co_await settings<json>([&] {
            return market_endpoint.get();
        });
        if (current.is_null() || current.at("host") != saved.at("host")
            || current.at("port") != saved.at("port")
            || current.at("fingerprint") != saved.at("fingerprint"))
            throw Error(ErrorCode::conflict, "market address changed during connection");
        market = std::move(next);
        co_return snapshot();
    });
    command("market.close", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        market.reset();
        co_return snapshot();
    });
    command("market.watchlist", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"instruments"});
        co_await settings<void>([&] {
            market_endpoint.select(p.at("instruments"));
        });
        co_return snapshot();
    });
    command("market.recording", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        if (!market)
            throw std::invalid_argument("select a market service first");
        const auto reader = market;
        auto status = co_await PollFuture{reader->recording_status()};
        if (market != reader)
            throw Error(ErrorCode::conflict, "market service selection changed during operation");
        auto result = read_published(json::object());
        result.values["recording"] = std::move(status);
        co_return result;
    });
    // Read-only: retain the selected client while its service request is suspended.
    command("market.minutes", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"venue", "symbol"});
        InstrumentId{text(p, "venue"), text(p, "symbol")}.validate();
        if (!market)
            throw std::invalid_argument("select a market service first");
        const auto reader = market;
        auto intraday = (co_await PollFuture{reader->minutes(text(p, "venue"), text(p, "symbol"))});
        // The series is that service's own observation. It is returned beside
        // what is published now only while that service is still the selected
        // one, and it says whose it is.
        if (market != reader)
            throw Error(
                ErrorCode::conflict,
                "market service selection changed during operation; inspect " "the original service before retrying");
        const auto source = reader->owner_read().header;
        intraday["service"] = source.at("service");
        intraday["instance_id"] = source.at("instance_id");
        auto result = read_published(json::object());
        result.values["intraday"] = std::move(intraday);
        co_return result;
    });
}

} // namespace asterion::terminal
