#pragma once
#include "settings.hpp"
#include <asterion/system/rpc_host.hpp>
#include <future>
#include <memory>

namespace asterion::warehouse {

// The Warehouse service: the composer that keeps the dataset current, and the
// readers that serve it and its acquisition tasks.
class WarehouseService
{
public:
    using Reply = service::RpcHost::Reply;

    explicit WarehouseService(Settings settings);
    ~WarehouseService();

    // One request frame from a client; `known` says its certificate is registered.
    Reply serve(std::string frame, bool known);
    void stop();
    std::shared_future<void> stopped() const;

private:
    struct Running;
    std::unique_ptr<Running> running_;
};

} // namespace asterion::warehouse
