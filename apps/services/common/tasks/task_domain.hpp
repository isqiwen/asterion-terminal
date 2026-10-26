#pragma once
#include <asterion/foundation/serialization.hpp>
#include <asterion/task.pb.h>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace asterion::tasks {

// One task kind of a service: where its definition and result live in the
// shared task messages and what its records are called.
struct KindSpec
{
    task::proto::TaskKind kind;
    std::string_view resource; // Resource kind of its retirement and abandonment.
    std::string_view manifest; // Persisted manifest type.
    std::string_view definition; // Field of Task.
    std::string_view result; // Field of TaskResponse.
    std::string_view finish; // Field of TaskFinish.
    // Projects a TaskExecution and carries editable experiment metadata.
    bool calculation = false;
    // Completes through a durable publication step instead of finish().
    bool publishes = false;
    // Its execution already holds the definition's data.
    bool execution_carries_input = false;
    // Its fixed input or output can reference a Data version.
    bool references_history = false;
};

// A secondary index over a persisted manifest field. Persisted schema: keep stable.
struct ManifestIndex
{
    std::string_view name, path;
};

// Records the store holds, for admission rules that relate a new task to others.
class Records
{
public:
    virtual ~Records() = default;
    // Throws Error(not_found) for an unknown id.
    virtual task::proto::StoredTaskRecord find(const std::string& id) const = 0;
    // Retired, or being retired.
    virtual bool retired(const std::string& id) const = 0;
    virtual std::optional<task::proto::StoredTaskRecord>
    same_input(task::proto::TaskKind, const std::string& input_digest) const = 0;
};

struct Admission
{
    task::proto::Task& task;
    const Records& records;
    const std::string& instance;
    const std::string& reuse_digest;
    // The admitted task with this id, when the submission repeats it.
    const task::proto::StoredTaskRecord* existing;
    // Set by the domain: an admitted input the new task shares instead of a copy.
    std::optional<task::proto::StoredTaskRecord> reusable;
};

// What one service's tasks mean. The Store owns lifecycle, attempts, files and
// recovery; the domain owns definitions, admission rules and results. Each
// service has exactly one, a static object passed to its Store.
class TaskDomain
{
public:
    virtual ~TaskDomain() = default;
    virtual std::string_view name() const = 0; // Resource domain of its records.
    virtual std::span<const KindSpec> kinds() const = 0;

    // Directories the service keeps beside its tasks; never task ids.
    virtual std::span<const std::string_view> directories() const
    {
        return {};
    }

    virtual std::span<const ManifestIndex> indexes() const
    {
        return {};
    }

    // Validates a submitted definition and sets its list summary and total.
    virtual void describe(task::proto::Task&) const = 0;

    // Checks references a parsed fixed input carries beside its definition.
    virtual void verify_input(const task::proto::Task&) const {}

    // Manifest fields beyond the shared ones. read_manifest receives exactly the
    // remaining fields and must reject any it does not expect.
    virtual void write_manifest(const task::proto::Task&, Json&) const = 0;
    virtual void
    read_manifest(const Json&, task::proto::Task&, const std::string& instance) const = 0;

    // The calculation descriptor of a fixed input; only for calculation kinds.
    virtual task::proto::TaskExecution execution(const task::proto::Task&, const std::string&) const
    {
        return {};
    }

    // Admitted identity an execution carries even when its bytes cannot execute.
    virtual void admitted_execution(const task::proto::Task&, task::proto::TaskExecution&) const {}

    virtual void
    validate_preparation(const task::proto::PreparedTaskSubmission&, const std::string&) const
    {}

    virtual void admit(Admission&) const {}

    // Rechecked just before registration, after file preparation.
    virtual void confirm_admission(const task::proto::Task&, const Records&) const {}

    // Whether a repeated submission names the same domain identity.
    virtual bool same_submission(const task::proto::Task&, const task::proto::Task&) const
    {
        return true;
    }

    // Captures executable evidence into a new task directory.
    virtual void capture(const task::proto::Task&, const std::filesystem::path&) const {}

    // Captured evidence of a stored task, relative to its directory, for cold backup.
    virtual std::vector<std::filesystem::path>
    captured(const task::proto::StoredTaskRecord&, const std::filesystem::path&, bool) const
    {
        return {};
    }

    virtual void validate_result(
        const task::proto::Task& input,
        const task::proto::TaskFinish&,
        const std::string& instance) const = 0;

    // Adds the reference a fixed input makes to a Data version, if any.
    virtual void history_references(
        const task::proto::Task&,
        const std::string&,
        data::proto::HistoryUsage&) const
    {}
};

} // namespace asterion::tasks
