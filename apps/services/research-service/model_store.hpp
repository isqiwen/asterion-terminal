#pragma once
#include "sqlite_database.hpp"
#include <asterion/research_model.pb.h>
#include <asterion/task.pb.h>
#include <asterion/foundation/time.hpp>
#include <memory>
#include <optional>
#include <vector>
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>

namespace asterion::tasks {

class Store;

} // namespace asterion::tasks

namespace asterion::research {

// Model semantics share the original Research journal; TaskStore owns execution.
class Models
{
public:
    Models(
        tasks::Store&,
        resource::proto::Owner,
        sqlite::Database::Access = sqlite::Database::Access::read_only,
        std::shared_ptr<const Clock> = std::make_shared<SystemClock>());
    std::optional<proto::ModelDraft> draft(const std::string&) const;
    proto::ModelPlan plan(const std::string&) const;
    proto::ModelMapping mapping(const std::string&) const;
    proto::ModelTimePlan time_plan(const std::string&) const;
    std::vector<proto::ModelTimePlan> time_plans(const std::string& after, unsigned limit) const;
    proto::ModelTimePlan
    capture_time(const proto::ModelTimeSave&, const task::proto::TaskResponse&) const;
    proto::ModelTimePlan
    time_admission(const resource::proto::Object&, const std::string& loaded_program) const;
    std::vector<proto::ModelMapping> mappings(const std::string& after, unsigned limit) const;
    proto::ModelMapping
    capture_mapping(const proto::ModelMappingSave&, const task::proto::TaskResponse&) const;
    std::vector<proto::ModelDraft> drafts(const std::string& after, unsigned limit) const;
    std::vector<proto::ModelPlan> plans(const std::string& after, unsigned limit) const;
    std::optional<proto::ModelReply> receipt(const proto::ModelMutation&) const;
    proto::ModelReply apply(
        const proto::ModelMutation&,
        const proto::ModelPlan* captured = nullptr,
        const proto::ModelMapping* mapping = nullptr,
        const proto::ModelTimePlan* time_plan = nullptr);
    proto::ModelPlan
    admission(const resource::proto::Object&, const std::string& loaded_program) const;
    std::vector<std::string> source_consumers(const std::string&) const;
    resource::proto::Catalogue resources(const resource::proto::CatalogueQuery&) const;
    static void inspect_backup(
        const std::filesystem::path&,
        const resource::proto::Owner&,
        std::span<const proto::ModelTrainingInput>,
        const std::function<void(const proto::ModelDraft&)>&,
        const std::function<void(const proto::ModelPlan&)>&,
        std::stop_token = {},
        const std::function<void(const proto::ModelMapping&)>& = {},
        const std::function<void(const proto::ModelTimePlan&)>& = {});

private:
    Models(
        sqlite::Database&,
        resource::proto::Owner,
        sqlite::Database::Access,
        std::shared_ptr<const Clock>);
    void verify_source(const proto::ModelPlan&, bool active) const;
    std::int64_t model_published_at(
        const resource::proto::Object&,
        const resource::proto::Object& plan,
        bool active) const;
    void verify_mapping(const proto::ModelMapping&, bool active) const;
    void verify_time_plan(const proto::ModelTimePlan&, bool active) const;
    sqlite::Database* database_;
    resource::proto::Owner owner_;
    bool writer_;
    std::shared_ptr<const Clock> clock_;
};

} // namespace asterion::research
