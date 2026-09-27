#include "walk_forward.hpp"
#include <stdexcept>
namespace asterion {
std::vector<FactorFoldRange> plan_factor_walk_forward(std::span<const std::int64_t> times,
                                                      unsigned warmup, unsigned horizon,
                                                      unsigned training, unsigned validation) {
  if (times.size() > 10000 || !warmup || warmup > 10000 || !horizon || horizon > 10000 ||
      training > 10000 || validation > 10000 || training < warmup + horizon + 30 ||
      validation < horizon + 30 || times.size() <= training ||
      (times.size() - training) % validation != 0)
    throw std::invalid_argument("walk-forward requires complete windows with at least 30 labelled "
                                "observations per partition");
  const auto count = (times.size() - training) / validation;
  if (count < 2 || count > 16)
    throw std::invalid_argument("walk-forward requires 2..16 validation folds");
  for (std::size_t i = 0; i < times.size(); ++i)
    if (times[i] < 0 || (i && times[i] < times[i - 1]))
      throw std::invalid_argument("invalid walk-forward event order");
  const auto boundary = [&](unsigned index) {
    if (index && index < times.size() && times[index - 1] == times[index])
      throw std::invalid_argument("walk-forward boundaries cannot split equal timestamps");
  };
  std::vector<FactorFoldRange> result;
  for (unsigned i = 0; i < count; ++i) {
    FactorFoldRange range{i * validation, i * validation + training,
                          (i + 1) * validation + training};
    boundary(range.training_begin);
    boundary(range.training_end);
    boundary(range.validation_end);
    result.push_back(range);
  }
  return result;
}
} // namespace asterion
