#pragma once
#include <asterion/base/serialization.hpp>
#include <asterion/trading.pb.h>
#include <memory>
#include "trading_connections.hpp"
#include "service_io.hpp"

namespace asterion::terminal {

class Application;

// All connection state and requests are advanced by the shared Native I/O owner.
class TradingClient
{
public:
    // Prepare transport resources on the calling management operation; initialization
    // and every subsequent request run asynchronously on ServiceIo.
    [[nodiscard]] static std::future<std::shared_ptr<TradingClient>>
    open(ServiceIo&, TradingEndpoint);
    [[nodiscard]] std::future<void> create(const Json& policy);
    ~TradingClient();
    TradingClient(const TradingClient&) = delete;
    TradingClient& operator=(const TradingClient&) = delete;
    [[nodiscard]] std::future<void> connect_broker();
    [[nodiscard]] std::future<void> disconnect_broker();
    [[nodiscard]] std::future<void> query_costs();
    [[nodiscard]] std::future<Json> view() const;
    TradingEndpoint endpoint() const;
    [[nodiscard]] std::future<void> execute(const Json& command);
    [[nodiscard]] std::future<Json> snapshot() const;

private:
    friend class Application;

    struct Read
    {
        std::shared_ptr<const Json> session;
        Json connection;
        Json account;
        bool failed = false;
        Json snapshot() const;
        Json render() const;
        bool operator==(const Read&) const = default;
    };

    // Application captures selected service views in one I/O owner turn.
    Read owner_view() const;
    TradingClient(ServiceIo&, TradingEndpoint);
    ServiceIo& io_;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace asterion::terminal
