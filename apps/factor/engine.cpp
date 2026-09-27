#include "factor_engine.hpp"
#include "momentum.hpp"
#include "walk_forward.hpp"
#include <array>
#include <asterion/domain/futures.hpp>
namespace asterion::factor {
namespace {
Instrument instrument(const protocol::v1::Contract &c) {
  return {{c.venue(), c.symbol()},
          AssetClass::futures,
          c.currency(),
          Decimal::from_raw(c.price_increment().units()),
          Decimal::from_raw(c.quantity_increment().units()),
          Decimal::from_raw(c.multiplier().units())};
}
TradeTick tick(const Instrument &spec, const protocol::v1::Tick &t) {
  return {spec.id, t.timestamp_ns(), Decimal::from_raw(t.price().units()),
          Decimal::from_raw(t.quantity().units())};
}
std::vector<FactorFoldRange> folds(const research::v1::FactorInput &input) {
  std::vector<std::int64_t> times;
  for (const auto &event : input.ticks())
    times.push_back(event.timestamp_ns());
  return plan_factor_walk_forward(
      times, input.lookbacks(input.lookbacks_size() - 1), input.horizon(),
      input.walk_forward().training_events(),
      input.walk_forward().validation_events());
}
} // namespace
void verify_result(const research::v1::FactorInput &input,
                   const research::v1::FactorResult &result) {
  protocol::validate_message(result);
  // The bounded first model is deterministic. Validate the complete claimed
  // output before committing it, not just its dataset label or summary.
  const auto expected = run(input);
  if (protocol::decode_factor_result(result) !=
      protocol::decode_factor_result(expected))
    throw std::invalid_argument(
        "factor result does not match its input and algorithm");
}
void validate(const research::v1::FactorInput &input) {
  static_cast<void>(protocol::decode_factor(input));
  const auto &c = input.contract();
  const auto spec = instrument(c);
  FuturesContract{spec, c.product(), c.delivery_month()}.validate();
  // Event-count horizons, not exchange calendar sessions or evenly timed bars.
  if (static_cast<unsigned>(input.ticks_size()) <
      input.lookbacks(input.lookbacks_size() - 1) + input.horizon() + 30)
    throw std::invalid_argument("factor analysis requires at least 30 labelled "
                                "observations after warmup and tail exclusion");
  if (input.has_holdout_start()) {
    const auto split = input.holdout_start();
    const auto count = static_cast<unsigned>(input.ticks_size());
    if (split < input.lookbacks(input.lookbacks_size() - 1) + input.horizon() +
                    30 ||
        count - split < input.horizon() + 30)
      throw std::invalid_argument(
          "each factor partition requires at least 30 labelled observations");
    if (input.ticks(static_cast<int>(split - 1)).timestamp_ns() >=
        input.ticks(static_cast<int>(split)).timestamp_ns())
      throw std::invalid_argument(
          "factor split must separate distinct timestamps");
  }
  if (input.has_walk_forward())
    static_cast<void>(folds(input));
  std::int64_t previous = -1;
  for (const auto &value : input.ticks()) {
    const auto t = tick(spec, value);
    t.validate(spec);
    if (t.price <= Decimal{} || t.timestamp_ns < previous)
      throw std::invalid_argument("invalid factor price or event order");
    previous = t.timestamp_ns;
  }
}
std::size_t work_units(const research::v1::FactorInput &input) {
  // Callers validate the bounded input first.
  if (input.has_walk_forward()) {
    const auto plan = folds(input);
    const auto training = input.walk_forward().training_events();
    return plan.size() *
           (training + input.walk_forward().validation_events() +
            (input.lookbacks_size() > 1 ? input.lookbacks_size() * training
                                        : 0));
  }
  return static_cast<std::size_t>(input.ticks_size()) +
         (input.lookbacks_size() > 1
              ? static_cast<std::size_t>(input.lookbacks_size()) *
                    input.holdout_start()
              : 0);
}
research::v1::FactorResult
run(const research::v1::FactorInput &input, std::stop_token stop,
    const std::function<void(std::size_t, std::size_t)> &progress) {
  validate(input);
  if (input.has_walk_forward()) {
    research::v1::FactorResult result;
    result.set_version(4);
    result.set_dataset_revision(input.dataset_revision());
    result.set_engine_version("asterion.factor.event-momentum.v4");
    result.set_horizon(input.horizon());
    result.set_input_count(input.ticks_size());
    result.set_evaluation_warmup(input.lookbacks(input.lookbacks_size() - 1));
    result.set_selection_rule(input.lookbacks_size() > 1
                                  ? "rolling_development_abs_spearman"
                                  : "rolling_fixed");
    const auto total = work_units(input);
    std::size_t completed = 0;
    for (const auto &range : folds(input)) {
      if (stop.stop_requested())
        throw std::runtime_error("factor analysis cancelled");
      auto slice = input;
      slice.set_holdout_start(range.training_end - range.training_begin);
      slice.clear_ticks();
      for (auto i = range.training_begin; i < range.validation_end; ++i)
        *slice.add_ticks() = input.ticks(i);
      slice.set_dataset_revision(protocol::factor_dataset_revision(slice));
      const auto evaluated =
          run(slice, stop, [&](std::size_t done, std::size_t) {
            if (progress)
              progress(completed + done, total);
          });
      completed += work_units(slice);
      auto *fold = result.add_folds();
      fold->set_training_begin(range.training_begin);
      fold->set_training_end(range.training_end);
      fold->set_validation_end(range.validation_end);
      fold->set_lookback(evaluated.lookback());
      *fold->mutable_candidates() = evaluated.candidates();
      *fold->mutable_development() = evaluated.partitions(0);
      *fold->mutable_holdout() = evaluated.partitions(1);
      for (auto *partition :
           {fold->mutable_development(), fold->mutable_holdout()}) {
        partition->set_begin_index(partition->begin_index() +
                                   range.training_begin);
        partition->set_end_index(partition->end_index() + range.training_begin);
      }
      result.set_purged_count(result.purged_count() + evaluated.purged_count());
      for (const auto &sample : evaluated.samples()) {
        if (sample.event_index() < slice.holdout_start())
          continue;
        auto *row = result.add_samples();
        *row = sample;
        row->set_event_index(sample.event_index() + range.training_begin);
      }
    }
    if (stop.stop_requested())
      throw std::runtime_error("factor analysis cancelled");
    return result;
  }
  const auto spec = instrument(input.contract());

  research::v1::FactorResult result;
  result.set_version(4);
  result.set_dataset_revision(input.dataset_revision());
  result.set_engine_version("asterion.factor.event-momentum.v4");

  result.set_horizon(input.horizon());
  result.set_input_count(static_cast<unsigned>(input.ticks_size()));
  std::array<std::vector<double>, 2> values, labels;
  const auto count = static_cast<std::size_t>(input.ticks_size());
  const auto warmup = input.lookbacks(input.lookbacks_size() - 1);
  result.set_evaluation_warmup(warmup);
  unsigned selected = input.lookbacks(0);
  const bool search = input.lookbacks_size() > 1;
  result.set_selection_rule(search ? "development_abs_spearman" : "fixed");
  const auto total = work_units(input);
  std::size_t completed = 0;
  auto advance = [&] {
    if (progress)
      progress(++completed, total);
  };
  if (search) {
    std::vector<MomentumCandidateScore> scores;
    // Candidate inputs and labels stop before the holdout boundary. All
    // candidates use exactly the same sample indices after common warmup.
    for (auto window : input.lookbacks()) {
      MomentumFactor candidate(spec, window);
      candidate.start();
      std::vector<double> features, outcomes;
      for (unsigned i = 0; i < input.holdout_start(); ++i) {
        if (stop.stop_requested())
          throw std::runtime_error("factor analysis cancelled");
        const auto event = tick(spec, input.ticks(static_cast<int>(i)));
        const auto feature = candidate.on_tick(event);
        if (feature && i >= warmup &&
            i + input.horizon() < input.holdout_start()) {
          features.push_back(*feature);
          outcomes.push_back(price_return(
              event.price,
              Decimal::from_raw(
                  input.ticks(static_cast<int>(i + input.horizon()))
                      .price()
                      .units())));
        }
        advance();
      }
      candidate.stop();
      const auto score = rank_correlation(features, outcomes);
      auto *evidence = result.add_candidates();
      evidence->set_lookback(window);
      evidence->set_sample_count(static_cast<unsigned>(features.size()));
      if (score)
        evidence->set_development_spearman(*score);
      scores.push_back({window, score});
    }
    const auto selection = select_momentum_lookback(scores);
    if (!selection)
      throw std::invalid_argument(
          "no candidate has a defined development correlation");
    selected = *selection;
  }
  result.set_lookback(selected);
  MomentumFactor factor(spec, selected);
  factor.start();
  for (std::size_t i = 0; i < count; ++i) {
    if (stop.stop_requested())
      throw std::runtime_error("factor analysis cancelled");
    const auto event = tick(spec, input.ticks(static_cast<int>(i)));
    const auto value = factor.on_tick(event);
    // Only the evaluator reads the future label; it never enters FactorPort.
    if (value && i >= warmup && i + input.horizon() < count) {
      const auto split =
          input.has_holdout_start() ? input.holdout_start() : count;
      if (i < split && i + input.horizon() >= split) {
        result.set_purged_count(result.purged_count() + 1);
        advance();
        continue;
      }
      const auto partition = i < split ? 0U : 1U;
      const auto &future = input.ticks(static_cast<int>(i + input.horizon()));
      const auto label =
          price_return(event.price, Decimal::from_raw(future.price().units()));
      auto *sample = result.add_samples();
      sample->set_event_index(static_cast<unsigned>(i));
      sample->set_timestamp_ns(event.timestamp_ns);
      sample->set_label_timestamp_ns(future.timestamp_ns());
      sample->set_value(*value);
      sample->set_forward_return(label);
      values[partition].push_back(*value);
      labels[partition].push_back(label);
    }
    advance();
  }
  factor.stop();
  if (stop.stop_requested())
    throw std::runtime_error("factor analysis cancelled");
  const unsigned partitions = input.has_holdout_start() ? 2U : 1U;
  for (unsigned index = 0; index < partitions; ++index) {
    auto *partition = result.add_partitions();
    partition->set_name(partitions == 1 ? "full_sample"
                        : index == 0    ? "development"
                                        : "holdout");
    partition->set_begin_index(index == 0 ? 0 : input.holdout_start());
    partition->set_end_index(index == 0 && partitions == 2
                                 ? input.holdout_start()
                                 : static_cast<unsigned>(count));
    partition->set_sample_count(static_cast<unsigned>(values[index].size()));
    if (const auto value = correlation(values[index], labels[index]))
      partition->set_pearson(*value);
    if (const auto value = rank_correlation(values[index], labels[index]))
      partition->set_spearman(*value);
  }
  return result;
}
} // namespace asterion::factor
