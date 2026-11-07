#pragma once
#include "settings.hpp"
#include <asterion/system/rpc_host.hpp>
#include <future>
#include <memory>

namespace asterion::market {

// The Market service: the market thread that owns the live market, serving
// quotes to every registered client.
class MarketService
{
public:
    using Reply = service::RpcHost::Reply;

    explicit MarketService(Settings settings);
    ~MarketService();

    // One request frame from a client; `known` says its certificate is registered.
    Reply serve(std::string frame, bool known);
    // Open quote streams end with their next frame.
    void drain();
    void stop();
    std::shared_future<void> stopped() const;

private:
    struct Running;
    std::unique_ptr<Running> running_;
};

} // namespace asterion::market
