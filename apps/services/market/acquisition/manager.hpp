#pragma once
#include "source.hpp"
#include <asterion/market.pb.h>
#include <asterion/system/thread_pool.hpp>
#include <functional>
#include <set>

namespace asterion::market_data::acquisition {
namespace wire = ::market;

// Market owns this object and every admitted task, independently of clients.
// A task takes one table from one source; at most one runs per source. Three
// workers serve clients and a fourth the contract snapshots. A full service refuses
// admission instead of accumulating an unbounded task queue.
class Manager
{
public:
    // `committed` runs after each commit to a source's store.
    Manager(
        std::filesystem::path directory,
        std::map<std::string, Factory> sources = {},
        std::function<void()> committed = {});
    ~Manager();
    wire::AcquisitionTask submit(wire::StartAcquisition);
    void cancel(const std::string& id);
    wire::AcquisitionTask get(const std::string& id) const;
    wire::AcquisitionTasks list(const wire::ListAcquisitionTasks&) const;
    wire::Sources sources() const;
    void stop();

private:
    struct Active
    {
        wire::AcquisitionTask task;
        std::stop_source cancel;
        std::shared_future<void> finished;
    };

    void run(Active&, Source&, warehouse::Store&, const wire::StartAcquisition&);
    void persist(const wire::AcquisitionTask&) const;
    void publish(const wire::AcquisitionTask&, Active&);
    void reap();
    wire::AcquisitionTask load(const std::filesystem::path&) const;
    std::filesystem::path directory_;
    std::map<std::string, Factory> sources_;
    std::map<std::string, std::unique_ptr<warehouse::Store>> stores_;
    std::function<void()> committed_;
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Active>> active_;
    bool stopped_ = false;
    ThreadPool workers_{4, 4};
};

} // namespace asterion::market_data::acquisition
