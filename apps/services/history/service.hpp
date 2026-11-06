#pragma once
#include "settings.hpp"
#include <asterion/system/rpc_host.hpp>
#include <future>
#include <memory>

namespace asterion::history {

// The History service: intake that takes each day's contract snapshot from disk,
// the composer that keeps the dataset current, and the readers that serve it
// and its acquisition tasks.
class HistoryService
{
public:
    using Reply = service::RpcHost::Reply;

    explicit HistoryService(Settings settings);
    ~HistoryService();

    // One request frame from a client; `can_manage` is absent for an unknown
    // certificate. Every call is answered where its route says.
    Reply serve(std::string frame, std::optional<bool> can_manage);
    void stop();
    std::shared_future<void> stopped() const;

private:
    struct Running;
    std::unique_ptr<Running> running_;
};

} // namespace asterion::history
