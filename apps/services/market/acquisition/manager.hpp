#pragma once
#include "source.hpp"
#include <asterion/market.pb.h>
#include <asterion/system/thread_pool.hpp>
#include <set>

namespace asterion::market_data::acquisition {
namespace wire = ::market;

// Market owns this object and every admitted task, independently of clients.
// Two fixed workers; at most one admitted task per configured source. A full
// service refuses admission instead of accumulating an unbounded task queue.
class Manager
{
public:
    Manager(std::filesystem::path directory, std::map<std::string, Factory> sources = {});
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
        std::set<std::string> providers;
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
    warehouse::Store research_;
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Active>> active_;
    bool stopped_ = false;
    ThreadPool workers_{2, 2};
};

} // namespace asterion::market_data::acquisition
