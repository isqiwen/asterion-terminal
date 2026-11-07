#pragma once
#include "service_endpoint.hpp"
#include "service_io.hpp"
#include <asterion/base/serialization.hpp>
#include <memory>

namespace asterion::terminal {

// One generated market.proto Request, as ProtoJSON, on its own pinned TLS
// connection; returns the Response as ProtoJSON. Errors are thrown.
[[nodiscard]] std::future<Json> call_market(ServiceIo&, ServiceEndpoint, Json);

// The selected Market service's watch stream, held for the page. Frames (LiveQuotes
// messages as ProtoJSON) wait in order until the page takes them; a page that
// stops taking them gets a new stream, whose first frame is complete.
class MarketWatch
{
public:
    [[nodiscard]] static std::future<std::shared_ptr<MarketWatch>>
    open(ServiceIo&, ServiceEndpoint);
    ~MarketWatch();
    MarketWatch(const MarketWatch&) = delete;
    MarketWatch& operator=(const MarketWatch&) = delete;
    // {frames, online, error}; called on the I/O owner. restart starts a new stream
    // at once, for a page that has no complete frame to apply deltas to. error is
    // why the stream last failed, until a frame arrives.
    Json take(bool restart);
    // Ends the stream for a page nobody can see, until take(true); called on the I/O owner.
    void pause();
    ServiceEndpoint endpoint() const;

private:
    struct Impl;
    explicit MarketWatch(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl_;
};

} // namespace asterion::terminal
