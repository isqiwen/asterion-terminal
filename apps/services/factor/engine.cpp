#include "factor_engine.hpp"
#include "evaluation.hpp"
#include "walk_forward.hpp"
#include <stdexcept>
namespace asterion::factor {
namespace {
void fill(factor::v1::FactorPartition& target, const FactorPartition& source, const char* name,
          std::size_t offset) {
  target.set_name(name);
  target.set_begin_index(static_cast<unsigned>(source.begin + offset));
  target.set_end_index(static_cast<unsigned>(source.end + offset));
  target.set_sample_count(static_cast<unsigned>(source.samples));
  if (source.pearson)
    target.set_pearson(*source.pearson);
  if (source.spearman)
    target.set_spearman(*source.spearman);
  if (source.pearson_ratio)
    target.set_pearson_ratio(*source.pearson_ratio);
  if (source.spearman_ratio)
    target.set_spearman_ratio(*source.spearman_ratio);
}
template <class Candidates>
void fill(Candidates& target, const std::vector<FactorCandidate>& source) {
  for (const auto& candidate : source) {
    auto* evidence = target.Add();
    evidence->set_lookback(candidate.lookback);
    evidence->set_sample_count(static_cast<unsigned>(candidate.samples));
    if (candidate.development_spearman)
      evidence->set_development_spearman(*candidate.development_spearman);
  }
}
} // namespace
factor::v1::FactorResult run(const factor::v1::FactorInput& input, std::stop_token stop,
                             const std::function<void(std::size_t, std::size_t)>& progress) {
  protocol::validate_factor_input(input);
  // From here on the factor sees ordered closes; what the series is made of
  // no longer matters.
  const auto observations = protocol::factor_observations(input);
  std::vector<std::span<const Decimal>> closes(observations.closes.begin(),
                                               observations.closes.end());
  // Price momentum reads the closes; the term structure the carry of each
  // observation, averaged over the window.
  const bool carry = input.factor() == protocol::v1::TERM_STRUCTURE;
  std::vector<std::span<const Decimal>> values(observations.terms.begin(),
                                               observations.terms.end());
  if (!carry)
    values = closes;
  const Feature feature = carry ? average : momentum;
  const std::vector<unsigned> lookbacks(input.lookbacks().begin(), input.lookbacks().end());
  const bool search = lookbacks.size() > 1;
  factor::v1::FactorResult result;
  result.set_version(6);
  result.set_dataset_revision(input.dataset_revision());
  result.set_engine_version(protocol::factor_engine_version);
  result.set_horizon(input.horizon());
  result.set_input_count(static_cast<unsigned>(observations.order.size()));
  result.set_evaluation_warmup(lookbacks.back());
  // Rows from `first` on, as observations of the whole input.
  const auto rows = [&](const FactorEvaluation& evaluation, std::size_t first, std::size_t offset) {
    for (const auto& row : evaluation.samples) {
      if (row.index < first)
        continue;
      auto* target = result.add_samples();
      target->set_event_index(static_cast<unsigned>(row.index + offset));
      target->set_value(row.value);
      target->set_forward_return(row.forward_return);
    }
    for (const auto& row : evaluation.cross_sections) {
      if (row.index < first)
        continue;
      auto* target = result.add_cross_sections();
      target->set_event_index(static_cast<unsigned>(row.index + offset));
      if (row.pearson)
        target->set_pearson(*row.pearson);
      if (row.spearman)
        target->set_spearman(*row.spearman);
    }
  };
  if (!input.has_walk_forward()) {
    const auto evaluation = evaluate_factor(closes, values, feature, lookbacks, input.horizon(),
                                            input.has_holdout_start()
                                                ? std::optional<std::size_t>(input.holdout_start())
                                                : std::nullopt,
                                            stop, progress);
    result.set_selection_rule(search ? "development_abs_spearman" : "fixed");
    result.set_lookback(evaluation.lookback);
    fill(*result.mutable_candidates(), evaluation.candidates);
    const bool holdout = evaluation.partitions.size() == 2;
    fill(*result.add_partitions(), evaluation.partitions[0],
         holdout ? "development" : "full_sample", 0);
    if (holdout)
      fill(*result.add_partitions(), evaluation.partitions[1], "holdout", 0);
    result.set_purged_count(static_cast<unsigned>(evaluation.purged));
    rows(evaluation, 0, 0);
    return result;
  }
  // Each fold is an evaluation of its own window: training observations
  // select, the validation observations that follow are scored.
  result.set_selection_rule(search ? "rolling_development_abs_spearman" : "rolling_fixed");
  const auto total = protocol::factor_work_units(input);
  std::size_t completed = 0;
  for (const auto& range : plan_factor_walk_forward(
           observations.order, lookbacks.back(), input.horizon(),
           input.walk_forward().training_events(), input.walk_forward().validation_events())) {
    const auto training = range.training_end - range.training_begin;
    std::size_t units = 0;
    const auto within = [&](std::vector<std::span<const Decimal>> all) {
      for (auto& series : all)
        series = series.subspan(range.training_begin, range.validation_end - range.training_begin);
      return all;
    };
    const auto evaluation =
        evaluate_factor(within(closes), within(values), feature, lookbacks, input.horizon(),
                        training, stop, [&](std::size_t done, std::size_t all) {
                          units = all;
                          if (progress)
                            progress(completed + done, total);
                        });
    completed += units;
    auto* fold = result.add_folds();
    fold->set_training_begin(range.training_begin);
    fold->set_training_end(range.training_end);
    fold->set_validation_end(range.validation_end);
    fold->set_lookback(evaluation.lookback);
    fill(*fold->mutable_candidates(), evaluation.candidates);
    fill(*fold->mutable_development(), evaluation.partitions[0], "development",
         range.training_begin);
    fill(*fold->mutable_holdout(), evaluation.partitions[1], "holdout", range.training_begin);
    result.set_purged_count(result.purged_count() + static_cast<unsigned>(evaluation.purged));
    rows(evaluation, training, range.training_begin);
  }
  return result;
}
} // namespace asterion::factor
