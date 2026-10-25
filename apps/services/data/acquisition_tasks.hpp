#pragma once
#include "data_store.hpp"
#include "material_store.hpp"
#include "task_store.hpp"
#include <asterion/kernel/thread_pool.hpp>
#include <asterion/kernel/rpc_host.hpp>
#include <atomic>
#include <array>
#include <algorithm>
#include <future>

namespace asterion::data {

// All acquisition state and journal access runs on DataHost's existing serial writer.
// The provider receives only an immutable attempt and cooperative cancellation.
class AcquisitionTasks
{
public:
    AcquisitionTasks(
        std::filesystem::path,
        std::string,
        std::string,
        Store&,
        std::string node,
        Materials* = nullptr);
    proto::AcquisitionTaskResponse handle(const proto::AcquisitionTaskRequest&);
    bool advance(bool admitting, bool stopping);
    std::vector<std::string> snapshot_holds(const std::string&);

private:
    Store& data_;
    const std::string service_, endpoint_, node_;
    const std::filesystem::path program_directory_;
    Materials* const materials_;
    tasks::Store tasks_;

    struct Execution
    {
        std::future<void> future;
        task::proto::TaskFinish result;
        std::optional<Materials::Prepared> material;
        std::string id, token;
        std::stop_source cancel;
        std::atomic<unsigned> progress{0};
    };

    // Preserve bounded overlapping acquisitions; journal/catalog transitions remain serial.
    std::array<Execution, 2> executions_;
    ThreadPool provider_{2, 2, service::wake_io_owner};
};

} // namespace asterion::data
