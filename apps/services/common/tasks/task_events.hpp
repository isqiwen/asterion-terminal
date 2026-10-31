#pragma once
#include <asterion/database/sqlite/sqlite_database.hpp>
#include <filesystem>
#include <functional>
#include <asterion/task.pb.h>

namespace asterion::tasks {

// Uses the task database's transaction for admission. Immutable segment bytes
// become durable before their references replace rows in the active log.
class TaskEvents
{
public:
    TaskEvents(sqlite::Database&, std::filesystem::path directory, bool read_only);
    using Visitor = std::function<void(const std::string&, std::uint64_t, const std::string&)>;
    void read(const Visitor&) const;
    void read_task(const std::string& task, const Visitor&) const;
    void rotate(); // Outside the caller's state transaction.
    void append(const std::string& task, std::uint64_t sequence, const std::string& body);

private:
    sqlite::Database& database_;
    std::filesystem::path directory_;
    bool read_only_;
    // One immutable segment, shared by adjacent per-task restore reads.
    mutable std::uint64_t cached_id_ = 0;
    mutable task::proto::StoredTaskEventSegment cached_segment_;
    const task::proto::StoredTaskEventSegment& segment(std::uint64_t id) const;
    task::proto::StoredTaskEventSegment live() const;
    std::filesystem::path path(std::uint64_t) const;
};

} // namespace asterion::tasks
