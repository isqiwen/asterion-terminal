#include "factor_engine.hpp"
#include "daily_momentum.hpp"
#include "momentum.hpp"
#include <array>
#include <stdexcept>
namespace asterion::factor {
research::v1::DailyFactorResult
run_daily(const research::v1::DailyFactorInput& input, std::stop_token stop,
          const std::function<void(std::size_t, std::size_t)>& progress) {
  protocol::validate_daily_factor(input);
  const auto bars = protocol::daily_factor_bars(input.dataset());
  const auto samples =
      daily_momentum_samples(bars, input.lookback(), input.horizon(), stop, progress);
  research::v1::DailyFactorResult result;
  result.set_version(1);
  result.set_dataset_revision(input.dataset_revision());
  result.set_engine_version("asterion.factor.daily-close-momentum.v1");
  result.set_lookback(input.lookback());
  result.set_horizon(input.horizon());
  result.set_input_count(static_cast<unsigned>(bars.size()));
  const auto split = input.has_holdout_start() ? input.holdout_start() : bars.size();
  std::array<std::vector<double>, 2> values, labels;
  for (const auto& sample : samples) {
    if (stop.stop_requested())
      throw std::runtime_error("daily momentum analysis cancelled");
    const auto index = sample.observation_index;
    if (index < split && index + input.horizon() >= split) {
      result.set_purged_count(result.purged_count() + 1);
      continue;
    }
    const auto partition = index < split ? 0U : 1U;
    auto* row = result.add_samples();
    row->set_observation_index(static_cast<unsigned>(index));
    row->set_trading_day(format_trading_date(sample.trading_day));
    row->set_label_day(format_trading_date(sample.label_day));
    row->set_value(sample.value);
    row->set_forward_return(sample.forward_return);
    values[partition].push_back(sample.value);
    labels[partition].push_back(sample.forward_return);
  }
  const unsigned count = input.has_holdout_start() ? 2 : 1;
  for (unsigned i = 0; i < count; ++i) {
    auto* part = result.add_partitions();
    part->set_name(count == 1 ? "full_sample" : i == 0 ? "development" : "holdout");
    part->set_begin_index(i ? static_cast<unsigned>(split) : 0);
    part->set_end_index(i ? result.input_count() : static_cast<unsigned>(split));
    part->set_sample_count(static_cast<unsigned>(values[i].size()));
    if (auto value = correlation(values[i], labels[i]))
      part->set_pearson(*value);
    if (auto value = rank_correlation(values[i], labels[i]))
      part->set_spearman(*value);
  }
  if (stop.stop_requested())
    throw std::runtime_error("daily momentum analysis cancelled");
  return result;
}
void verify_daily_result(const research::v1::DailyFactorInput& input,
                         const research::v1::DailyFactorResult& result) {
  protocol::validate_message(result);
  if (run_daily(input).SerializeAsString() != result.SerializeAsString())
    throw std::invalid_argument("daily factor result does not match its input and algorithm");
}
} // namespace asterion::factor
