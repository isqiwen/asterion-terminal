#pragma once
#include "service_endpoint.hpp"
#include "service_io.hpp"
#include <asterion/base/serialization.hpp>
#include <memory>

namespace asterion::terminal {

// One generated quotes.proto Request, as ProtoJSON, on its own pinned TLS
// connection; returns the Response as ProtoJSON. Errors are thrown.
[[nodiscard]] std::future<Json> call_quotes(ServiceIo&, ServiceEndpoint, Json);

// The selected Quotes service's watch stream, held for the page. Frames (LiveQuotes
// messages as ProtoJSON) wait in order until the page takes them; a page that
// stops taking them gets a new stream, whose first frame is complete.
class QuotesWatch
{
public:
    [[nodiscard]] static std::future<std::shared_ptr<QuotesWatch>>
    open(ServiceIo&, ServiceEndpoint);
    ~QuotesWatch();
    QuotesWatch(const QuotesWatch&) = delete;
    QuotesWatch& operator=(const QuotesWatch&) = delete;
    // {frames, online}; called on the I/O owner. restart starts a new stream at
    // once, for a page that has no complete frame to apply deltas to.
    Json take(bool restart);
    ServiceEndpoint endpoint() const;

private:
    struct Impl;
    explicit QuotesWatch(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl_;
};

} // namespace asterion::terminal
