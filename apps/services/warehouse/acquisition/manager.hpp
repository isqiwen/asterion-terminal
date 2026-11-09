#pragma once
#include "source.hpp"
#include "composition.hpp"
#include <asterion/warehouse.pb.h>
#include <asterion/system/thread_pool.hpp>
#include <functional>
#include <set>

namespace asterion::warehouse::acquisition {
namespace wire = ::warehouse;

// Warehouse owns this object and every admitted task, independently of clients.
// A task takes one table through the sources in their order of priority, each
// filling only what the ones before it lack. Tasks run one at a time, and a full
// queue refuses admission.
class Manager
{
public:
    // `finished` runs when a task ends, however it ends.
    Manager(
        std::filesystem::path directory,
        std::map<std::string, Factory> sources = {},
        std::function<void()> finished = {});
    ~Manager();
    wire::AcquisitionTask submit(wire::StartAcquisition);
    void cancel(const std::string& id);
    wire::AcquisitionTask get(const std::string& id) const;
    wire::AcquisitionTasks list(const wire::ListAcquisitionTasks&) const;
    wire::AcquisitionOptions options() const;
    void stop();

private:
    struct Active
    {
        wire::AcquisitionTask task;
        std::stop_source cancel;
        std::shared_future<void> finished;
    };

    void work(Active&, const wire::StartAcquisition&, const std::vector<std::string>& providers);
    void run(
        Active&,
        Source&,
        warehouse::Store&,
        const wire::StartAcquisition&,
        const std::set<std::string>& covered);
    void persist(const wire::AcquisitionTask&) const;
    void publish(const wire::AcquisitionTask&, Active&);
    void reap();
    wire::AcquisitionTask load(const std::filesystem::path&) const;
    std::filesystem::path directory_;
    std::map<std::string, Factory> sources_;
    std::map<std::string, std::unique_ptr<warehouse::Store>> stores_;
    std::function<void()> finished_;
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Active>> active_;
    bool stopped_ = false;
    ThreadPool workers_{1, 16};
};

} // namespace asterion::warehouse::acquisition
