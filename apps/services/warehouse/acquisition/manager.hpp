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
    // `finished` receives the source versions committed by the task, however it ends.
    Manager(
        std::filesystem::path directory,
        std::map<std::string, Factory> sources = {},
        std::function<void(const SourceVersions&)> finished = {});
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
        SourceVersions versions;
    };

    // What a task covers once its instruments are resolved: days of whole
    // exchanges or, for minutes, days of each contract.
    struct Scope
    {
        std::vector<std::string> exchanges;
        std::chrono::sys_days first, last;
        std::vector<std::tuple<std::string, std::chrono::sys_days, std::chrono::sys_days>>
            contracts;
    };

    void work(Active&, const wire::StartAcquisition&, const std::vector<std::string>& providers);
    Scope scope(Active&, const wire::StartAcquisition&);
    std::vector<Contract> listed(Active&, const std::string& exchange);
    void
    run(Active&, Source&, warehouse::Store&, const Scope&, const std::set<std::string>& covered);
    void persist(const wire::AcquisitionTask&) const;
    void publish(const wire::AcquisitionTask&, Active&);
    void reap();
    wire::AcquisitionTask load(const std::filesystem::path&) const;
    std::filesystem::path directory_;
    std::map<std::string, Factory> sources_;
    std::map<std::string, std::unique_ptr<warehouse::Store>> stores_;
    std::function<void(const SourceVersions&)> finished_;
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Active>> active_;
    bool stopped_ = false;
    ThreadPool workers_{1, 16};
};

} // namespace asterion::warehouse::acquisition
