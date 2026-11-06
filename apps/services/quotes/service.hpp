#pragma once
#include "settings.hpp"
#include <asterion/system/rpc_host.hpp>
#include <future>
#include <memory>

namespace asterion::quotes {

// The Quotes service: the market thread that owns the live market, serving
// quotes to every registered client.
class QuotesService
{
public:
    using Reply = service::RpcHost::Reply;

    explicit QuotesService(Settings settings);
    ~QuotesService();

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

} // namespace asterion::quotes
