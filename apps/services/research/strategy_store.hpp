#pragma once
#include "sqlite_database.hpp"
#include <asterion/research_strategy.pb.h>
#include <asterion/task.pb.h>
#include <asterion/foundation/time.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

namespace asterion::tasks {

class Store;

} // namespace asterion::tasks

namespace asterion::research {

proto::StrategyPackage capture_strategy_package(
    const resource::proto::Owner&,
    const proto::StrategyFreeze&,
    const task::proto::TaskResponse&,
    const std::string& verified_input_digest);

proto::StrategyPackage capture_logic_strategy_package(
    const resource::proto::Owner&,
    const proto::StrategyLogicFreeze&,
    const task::proto::TaskResponse&,
    const std::string& verified_input_digest);

// The caller retains the Research root and serializes this borrowed metadata
// connection on its existing journal. No task scheduling or Data writes here.
class Strategies
{
public:
    Strategies(
        tasks::Store&,
        resource::proto::Owner,
        sqlite::Database::Access = sqlite::Database::Access::read_only,
        std::shared_ptr<const Clock> = std::make_shared<SystemClock>());
    std::optional<proto::StrategyReply> receipt(const proto::StrategyMutation&) const;
    // A new freeze accepts only the internally captured, verified source record.
    // Replaying its durable receipt needs neither the source files nor Data.
    proto::StrategyReply
    apply(const proto::StrategyMutation&, const proto::StrategyPackage* captured = nullptr);
    proto::StrategyPackage package(const std::string&) const;
    proto::StrategyDelivery delivery(const std::string&) const;
    proto::StrategyReply delivery_status(const std::string&) const;
    std::vector<proto::StrategyPackage> packages(const std::string& after, unsigned limit) const;
    std::vector<proto::StrategyDelivery>
    deliveries(const std::string& package, const std::string& after, unsigned limit) const;
    std::vector<std::string> source_consumers(const std::string&) const;
    void
    verify_use(const proto::StrategyUse&, const std::string& target_task, bool admission) const;
    resource::proto::Catalogue resources(const resource::proto::CatalogueQuery&) const;
    static void inspect_backup(
        const std::filesystem::path&,
        const resource::proto::Owner&,
        std::span<const std::pair<std::string, proto::StrategyUse>>,
        const std::function<void(const proto::StrategyPackage&)>&,
        const std::function<void(const proto::StrategyDelivery&)>&,
        std::stop_token = {});

private:
    Strategies(
        sqlite::Database&,
        resource::proto::Owner,
        sqlite::Database::Access,
        std::shared_ptr<const Clock>);
    void verify_source(const proto::StrategyPackage&, bool admission) const;
    sqlite::Database* database_;
    resource::proto::Owner owner_;
    bool writer_;
    std::shared_ptr<const Clock> clock_;
};

} // namespace asterion::research
