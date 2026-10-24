#pragma once
#include "notice_store.hpp"
#include <chrono>
#include <asterion/kernel/rpc_host.hpp>
#include <asterion/kernel/polled_task.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include <asterion/data_service.pb.h>

namespace asterion::intelligence {

class NoticeHost
{
public:
    struct Configuration
    {
        std::filesystem::path directory;
        resource::proto::Owner owner;
        std::string discovery_endpoint, program_artifact;
    };

    explicit NoticeHost(Configuration);
    void open();
    service::RpcHost::Reply accept(std::string, bool health = false);
    bool advance(service::RpcHost::Stage);

private:
    Configuration configuration_;
    std::string process_;
    std::unique_ptr<Store> store_;
    programs::Files::Entry programme_;
    ThreadPool writer_{1, 8, service::wake_io_owner};
    ThreadPool readers_{2, 8, service::wake_io_owner};
    std::future<void> opening_;
    bool initialized_ = false, quiescing_ = false;

    template <class F>
    auto write(F f)
    {
        return PollFuture{writer_.submit([f = std::move(f)](std::stop_token) {
            f();
        })};
    }

    template <class F>
    auto read(F f)
    {
        return PollFuture{readers_.submit(std::move(f))};
    }

    data::proto::DataResponse data_call(
        const resource::proto::Owner&,
        data::proto::DataRequest,
        std::stop_token,
        std::chrono::steady_clock::time_point = std::chrono::steady_clock::time_point::max()) const;
    PolledTask<proto::NoticeRecord> submit(proto::NoticeCandidate);
    PolledTask<proto::NoticeChangeRecord> change(proto::NoticeChangeRequest);
    PolledTask<std::string> serve(std::string, bool);
};

} // namespace asterion::intelligence
