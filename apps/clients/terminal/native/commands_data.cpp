#include "application_impl.hpp"

namespace asterion::terminal {

PolledTask<ServiceEndpoint>
Application::Impl::address_of(const SavedEndpoint& saved_endpoint, const char* name)
{
    const auto saved = co_await settings<json>([&] {
        return saved_endpoint.get();
    });
    if (saved.is_null())
        throw std::invalid_argument(std::string("configure the ") + name + " address first");
    const auto tls = co_await settings<ipc::TlsIdentity>([&] {
        return trading_connections.tls(saved.at("fingerprint"));
    });
    co_return ServiceEndpoint{
        saved.at("host").get<std::string>(),
        name,
        saved.at("port").get<std::uint16_t>(),
        {},
        tls
    };
}

void Application::Impl::register_data_commands()
{
    // One quotes.proto Request to the saved service, relayed unchanged.
    command("quotes.call", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"request"});
        const auto address = co_await address_of(quotes_endpoint, "Quotes");
        auto reply = co_await PollFuture{call_quotes(service_io, address, p.at("request"))};
        // A settings edit meanwhile means the reply may be from another service.
        if (!same_service_endpoint(co_await address_of(quotes_endpoint, "Quotes"), address))
            throw Error(
                ErrorCode::conflict,
                "service selection changed during operation; inspect the original service");
        co_return Response{json{{"quotes", std::move(reply)}}};
    });
    // One history.proto Request to the saved service, relayed unchanged.
    command("history.call", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"request"});
        const auto address = co_await address_of(history_endpoint, "History");
        auto reply = co_await PollFuture{call_history(service_io, address, p.at("request"))};
        if (!same_service_endpoint(co_await address_of(history_endpoint, "History"), address))
            throw Error(
                ErrorCode::conflict,
                "service selection changed during operation; inspect the original service");
        co_return Response{json{{"history", std::move(reply)}}};
    });
    command("quotes.open", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        const auto address = co_await address_of(quotes_endpoint, "Quotes");
        auto next = co_await connect<std::shared_ptr<QuotesWatch>>(
            [this, address]() -> PolledTask<std::shared_ptr<QuotesWatch>> {
                co_return co_await PollFuture{QuotesWatch::open(service_io, address)};
            });
        if (!same_service_endpoint(co_await address_of(quotes_endpoint, "Quotes"), address))
            throw Error(ErrorCode::conflict, "Quotes address changed during connection");
        watch = std::move(next);
        co_return snapshot();
    });
    command("quotes.close", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        watch.reset();
        co_return snapshot();
    });
    // The frames of the open watch since the last call; null when none is open.
    command("quotes.watch", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"restart"});
        const bool restart = p.at("restart").get<bool>();
        co_return Response{json{{"quotes_watch", watch ? watch->take(restart) : json(nullptr)}}};
    });
    command("quotes.pause", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        if (watch)
            watch->pause();
        co_return Response{json::object()};
    });
    command("quotes.watchlist", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"instruments"});
        co_await settings<void>([&] {
            quotes_endpoint.select(p.at("instruments"));
        });
        co_return snapshot();
    });
}

} // namespace asterion::terminal
