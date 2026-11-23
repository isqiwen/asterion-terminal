#pragma once
#include "exchange_rules.hpp"
#include "source.hpp"
#include "composition.hpp"
#include "freshness.hpp"
#include <asterion/warehouse.pb.h>
#include <asterion/system/thread_pool.hpp>
#include <condition_variable>
#include <functional>
#include <set>

namespace asterion::warehouse::acquisition {
namespace wire = ::warehouse;

// Warehouse owns this object and every admitted task, independently of clients.
// Market-bar tasks acquire their prerequisite calendar, contract and settlement
// tables first; each table goes through its sources in priority order, filling
// only what earlier ones lack. Tasks run one at a time, and a full queue
// refuses admission. Market-bar tasks go first: any other task gives way after
// the partition it is on and continues where it stopped.
class Manager
{
public:
    // `finished` receives the source versions committed by the task, however it ends.
    Manager(
        std::filesystem::path directory,
        std::map<std::string, Factory> sources = {},
        std::function<void(const SourceVersions&)> finished = {},
        std::vector<PriceTick> verified_price_ticks = {});
    ~Manager();
    wire::AcquisitionTask submit(wire::StartAcquisition);
    void cancel(const std::string& id);
    wire::AcquisitionTask get(const std::string& id) const;
    wire::AcquisitionTasks list(const wire::ListAcquisitionTasks&) const;
    wire::AcquisitionOptions options() const;
    // Whether a task of this table is queued or running.
    bool busy(wire::Table) const;
    void stop();

private:
    struct Active
    {
        wire::AcquisitionTask task;
        std::stop_source cancel;
        std::shared_future<void> finished;
        SourceVersions versions;
        std::map<std::string, std::unique_ptr<Source>> sources;
        bool has_turn = false;
    };

    // What a task covers once its instruments are resolved: days of whole
    // exchanges or, for minutes, days of each contract.
    struct Scope
    {
        std::vector<std::string> exchanges;
        std::chrono::sys_days first, last;
        std::vector<std::tuple<std::string, std::chrono::sys_days, std::chrono::sys_days>>
            contracts;
        std::set<std::string> listed_exchanges;
    };

    void work(Active&, const wire::StartAcquisition&);
    // Waits for the one running slot; a cancelled task returns without it.
    void take_turn(Active&);
    void give_turn(Active&);
    // With `mutex_` held.
    bool market_waiting() const;
    std::set<std::string> planned_keys(
        Active&,
        const std::string& name,
        const Scope&,
        bool refetch,
        const std::set<std::string>& unavailable,
        const warehouse::Version& held_calendar,
        const warehouse::OpenDays& open,
        std::chrono::sys_days today);
    bool acquire_table(
        Active&,
        const std::string& name,
        const Scope&,
        bool refetch,
        bool primary,
        const std::set<std::string>& planned,
        std::chrono::sys_days today,
        std::set<std::string>& unavailable,
        std::optional<wire::AcquisitionTask>& failure);
    Scope scope(Active&, const wire::StartAcquisition&);
    Source& source(Active&, const std::string& provider);
    std::vector<Contract> listed(Active&, const std::string& exchange);
    // Keys in `answered` every source found empty before: taken again only when
    // refetching. This source's empty answers are added to `empty`.
    void run(
        Active&,
        const std::string& name,
        bool refetch,
        bool primary,
        Source&,
        warehouse::Store&,
        const Scope&,
        const std::set<std::string>& planned,
        std::set<std::string>& counted,
        const std::map<std::string, std::string>& last_provider,
        std::chrono::sys_days today,
        const std::set<std::string>& covered,
        const std::set<std::string>& answered,
        std::set<std::string>& empty);
    // The calendar the first calendar source holds now, and its open days
    // through yesterday; empty without one.
    std::pair<warehouse::Version, warehouse::OpenDays> calendar() const;
    void persist(const wire::AcquisitionTask&) const;
    void publish(const wire::AcquisitionTask&, Active&);
    void reap();
    wire::AcquisitionTask load(const std::filesystem::path&) const;
    std::filesystem::path directory_;
    std::map<std::string, Factory> sources_;
    std::map<std::string, std::unique_ptr<warehouse::Store>> stores_;
    std::function<void(const SourceVersions&)> finished_;
    std::vector<PriceTick> verified_price_ticks_;
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Active>> active_;
    bool stopped_ = false;
    std::condition_variable_any turn_;
    bool turn_taken_ = false;
    ThreadPool workers_{1, 16}, background_{1, 16};
};

} // namespace asterion::warehouse::acquisition
