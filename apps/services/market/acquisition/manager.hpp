#pragma once
#include "source.hpp"
#include <asterion/acquisition.pb.h>
#include <asterion/system/thread_pool.hpp>

namespace asterion::market_data::acquisition {
namespace wire = asterion::acquisition::proto;

// Market owns this object and every admitted task, independently of clients.
// Two fixed workers; at most one admitted task per configured source. A full
// service refuses admission instead of accumulating an unbounded task queue.
class Manager
{
public:
    Manager(std::filesystem::path directory, std::map<std::string, Factory> sources = {});
    ~Manager();
    wire::Task submit(wire::Request);
    void cancel(const std::string& id);
    wire::Task get(const std::string& id) const;
    wire::Tasks list(const wire::TasksQuery&) const;
    wire::Sources sources() const;
    void stop();

private:
    struct Active
    {
        wire::Task task;
        std::stop_source cancel;
        std::shared_future<void> finished;
    };

    void run(Active&, Source&, warehouse::Store&);
    void persist(const wire::Task&) const;
    void publish(const wire::Task&, Active&);
    void reap();
    wire::Task load(const std::filesystem::path&) const;
    std::filesystem::path directory_;
    std::map<std::string, Factory> sources_;
    std::map<std::string, std::unique_ptr<warehouse::Store>> stores_;
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Active>> active_;
    bool stopped_ = false;
    ThreadPool workers_{2, 2};
};

} // namespace asterion::market_data::acquisition
