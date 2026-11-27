#pragma once
#include "service_endpoint.hpp"
#include "service_io.hpp"
#include <asterion/base/serialization.hpp>
#include <memory>
#include <string>
#include <vector>

namespace asterion::terminal {

// One generated market.proto Request, as ProtoJSON, over pinned mutual TLS;
// returns the Response as ProtoJSON. Errors are thrown.
[[nodiscard]] std::future<Json> call_market(ServiceIo&, ServiceEndpoint, Json);
[[nodiscard]] std::future<std::string> call_market_json(ServiceIo&, ServiceEndpoint, Json);

// The selected Market service's watch stream, held for the page. Binary
// QuoteFrame payloads wait in order until the page takes them; a page that
// stops taking them gets a new stream, whose first frame is complete.
class MarketWatch
{
public:
    struct Batch
    {
        std::vector<std::string> frames;
        bool online = false;
        std::string error;
    };

    [[nodiscard]] static std::future<std::shared_ptr<MarketWatch>>
    open(ServiceIo&, ServiceEndpoint);
    ~MarketWatch();
    MarketWatch(const MarketWatch&) = delete;
    MarketWatch& operator=(const MarketWatch&) = delete;
    // {frames, online, error}; called on the I/O owner. restart starts a new stream
    // at once, for a page that has no complete frame to apply deltas to. error is
    // why the stream last failed, until a frame arrives.
    Batch take_binary(bool restart);
    Json take(bool restart);
    std::string take_json(bool restart);
    ServiceEndpoint endpoint() const;

private:
    struct Impl;
    explicit MarketWatch(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl_;
};

} // namespace asterion::terminal
