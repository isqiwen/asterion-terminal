#pragma once
#include <asterion/protocol/factor.hpp>
namespace asterion::data {
// The Data service resolves published records before calling this file reader.
// Runs on its bounded file pool, never on a task-state thread.
factor::v1::DailyFactorDataset daily_factor_dataset(const data::v1::HistoryRecord& source);
} // namespace asterion::data
