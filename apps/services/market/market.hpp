#pragma once
#include "settings.hpp"
#include <asterion/system/rpc_host.hpp>
#include <future>
#include <memory>

namespace asterion::market_data {

// The Market service: the market thread that owns the live market and its
// recording; intake that takes finished recording files from disk; the composer
// that keeps research current; and the reader pool that serves datasets and
// acquisition tasks.
class Market
{
public:
    using Reply = service::RpcHost::Reply;

    Market(Settings settings, std::string instance);
    ~Market();

    // One request frame from a client; `can_manage` is absent for an unknown
    // certificate. Every call is answered where its route says.
    Reply serve(std::string frame, std::optional<bool> can_manage);
    // Open quote streams end with their next frame.
    void drain();
    void stop();
    std::shared_future<void> stopped() const;

private:
    struct Running;
    std::unique_ptr<Running> running_;
};

} // namespace asterion::market_data
