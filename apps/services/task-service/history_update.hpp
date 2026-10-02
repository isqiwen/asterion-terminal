#pragma once
#include "history_archive.hpp"
#include <asterion/v1/data.pb.h>
namespace asterion::tasks {
// Reads and verifies immutable files outside the task-state lock. Clock is
// explicit for deterministic validation of completed calendar-day boundaries.
data::v1::HistoryUpdatePlan history_update_plan(const history_files::Archive&,
                                                const data::v1::HistoryUpdateQuery&,
                                                std::int64_t now_ns);
} // namespace asterion::tasks
