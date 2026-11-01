#pragma once
#include "service_endpoint.hpp"
#include "service_io.hpp"
#include "market_snapshot.hpp"
#include <asterion/base/serialization.hpp>
#include <memory>

namespace asterion::terminal {

class Application;

class MarketClient
{
public:
    [[nodiscard]] static std::future<std::shared_ptr<MarketClient>>
    open(ServiceIo&, ServiceEndpoint);
    // Bounded one-shot data RPC, independent of the selected live watch. The
    // generated Protobuf JSON mapping carries the UI's request without a
    // second set of hand-written business fields.
    [[nodiscard]] static std::future<Json> data(ServiceIo&, ServiceEndpoint, Json);
    ~MarketClient();
    MarketClient(const MarketClient&) = delete;
    MarketClient& operator=(const MarketClient&) = delete;
    // Current trading-day minute bars observed by the market service.
    [[nodiscard]] std::future<Json> minutes(const std::string& venue, const std::string& symbol);
    [[nodiscard]] std::future<Json> recording_status();
    [[nodiscard]] std::future<Json> snapshot() const;
    ServiceEndpoint endpoint() const;

private:
    friend class Application;
    // Capture selected service data in one I/O owner turn.
    MarketProjection owner_read() const;
    MarketClient(ServiceIo&, ServiceEndpoint);
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace asterion::terminal
