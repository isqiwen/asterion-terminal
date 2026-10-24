#pragma once
#include "sqlite_database.hpp"
#include <asterion/research_batch.pb.h>
#include <asterion/research_assessment.pb.h>
#include <asterion/task.pb.h>
#include <asterion/foundation/time.hpp>
#include <memory>
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
#include <asterion/resource.pb.h>
#include <optional>
#include <vector>

namespace asterion::tasks {

class Store;

} // namespace asterion::tasks

namespace asterion::research {

// Borrowed domain connection, used only on Research's existing serial journal.
class Batches
{
public:
    Batches(
        tasks::Store&,
        resource::proto::Owner,
        sqlite::Database::Access = sqlite::Database::Access::read_only,
        std::shared_ptr<const Clock> = std::make_shared<SystemClock>());
    std::optional<proto::BacktestBatchState> receipt(const proto::BacktestBatchMutation&) const;
    proto::BacktestBatchState apply(
        const proto::BacktestBatchMutation&,
        const proto::BacktestBatchDefinition* captured = nullptr);
    proto::BacktestBatchState state(const std::string&) const;
    std::vector<task::proto::Task> member_tasks(const proto::BacktestBatchState&) const;
    std::vector<proto::BacktestBatchState>
    states(const std::string& after, unsigned limit, bool active_only = false) const;
    proto::BacktestBatchState begin_member(
        const std::string& batch,
        const std::string& task,
        const std::string& input_digest);
    proto::BacktestBatchState confirm_member(const std::string& batch, const std::string& task);
    proto::BacktestBatchState
    reject_member(const std::string& batch, const std::string& task, const protocol::proto::Error&);
    proto::BacktestBatchState
    finish(const std::string& batch, std::optional<std::uint64_t> elapsed_ms);
    std::vector<proto::BacktestBatchState> interrupt_running();
    std::vector<std::string> source_consumers(const std::string&) const;
    std::vector<std::string> assessment_consumers(const std::string&) const;
    void verify_admission(
        const resource::proto::Object&,
        const std::string& task,
        const std::string& input_digest) const;

    std::optional<proto::BatchAssessment> assessment_receipt(
        const proto::BatchAssessmentMutation&) const;
    void verify_assessment_snapshot(const proto::BatchAssessmentSnapshot&) const;
    proto::BatchAssessment save_assessment(
        const proto::BatchAssessmentMutation&,
        const proto::BatchAssessmentSnapshot&,
        const std::string& recording_program);
    proto::BatchAssessment assessment(const std::string&) const;
    std::vector<proto::BatchAssessment>
    assessments(const std::string& batch, const std::string& after, unsigned limit) const;
    static void inspect_assessments(
        const std::filesystem::path&,
        const resource::proto::Owner&,
        const std::function<void(const proto::BatchAssessment&)>&,
        std::stop_token = {});

    resource::proto::Catalogue resources(const resource::proto::CatalogueQuery&) const;
    static void inspect_backup(
        const std::filesystem::path&,
        const resource::proto::Owner&,
        std::span<const task::proto::StoredTaskRecord>,
        const std::function<void(const proto::BacktestBatchState&)>&,
        std::stop_token = {});

private:
    Batches(
        sqlite::Database&,
        resource::proto::Owner,
        sqlite::Database::Access,
        std::shared_ptr<const Clock>);
    void writable() const;

    void persist(const proto::BacktestBatchState&);
    void verify_source(const proto::BacktestBatchDefinition&, bool active) const;
    void verify_member(const proto::BacktestBatchState&, const std::string&) const;
    sqlite::Database* database_;
    resource::proto::Owner owner_;
    bool writer_;
    std::shared_ptr<const Clock> clock_;
};

} // namespace asterion::research
