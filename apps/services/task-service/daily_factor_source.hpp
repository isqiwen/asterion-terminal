#pragma once
#include <asterion/protocol/research.hpp>
namespace asterion::tasks {
// Source arguments must be copied from Store, never accepted as client evidence.
// Performs bounded disk verification outside the Task Service business lock.
research::v1::DailyFactorDataset daily_factor_dataset(const data::v1::HistoryRecord& source);
} // namespace asterion::tasks
