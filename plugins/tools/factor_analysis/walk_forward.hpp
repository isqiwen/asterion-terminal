#pragma once
#include <cstdint>
#include <span>
#include <vector>
namespace asterion {
struct FactorFoldRange {
  unsigned training_begin, training_end, validation_end;
};
// Fixed-width, chronological folds. Validation ranges do not overlap. Every
// event after the initial training window belongs to exactly one fold.
std::vector<FactorFoldRange> plan_factor_walk_forward(std::span<const std::int64_t> timestamps,
                                                      unsigned warmup, unsigned horizon,
                                                      unsigned training_events,
                                                      unsigned validation_events);
} // namespace asterion
