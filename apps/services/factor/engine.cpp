#include "factor_engine.hpp"
#include "evaluation.hpp"
#include "universe.hpp"
#include <algorithm>
#include "walk_forward.hpp"
#include <stdexcept>

namespace asterion::factor {
namespace {

void fill(
    factor::proto::FactorPartition& target,
    const FactorPartition& source,
    const char* name,
    std::size_t offset)
{
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
    for (const auto& spread : source.spreads) {
        auto* row = target.add_spreads();
        row->set_highest(spread.highest);
        row->set_lowest(spread.lowest);
        row->set_observations(static_cast<unsigned>(spread.observations));
    }
}

template <class Candidates>
void fill(Candidates& target, const std::vector<FactorCandidate>& source)
{
    for (const auto& candidate : source) {
        auto* evidence = target.Add();
        evidence->set_lookback(candidate.lookback);
        evidence->set_sample_count(static_cast<unsigned>(candidate.samples));
        if (candidate.development_spearman)
            evidence->set_development_spearman(*candidate.development_spearman);
    }
}

} // namespace

factor::proto::FactorResult run(
    const factor::proto::FactorInput& input,
    std::stop_token stop,
    const std::function<void(std::size_t, std::size_t)>& progress)
{
    // From here on the factor sees ordered closes; what the series is made of
    // no longer matters.
    const auto observations = protocol::validate_factor_input(input);
    using Series = std::span<const std::optional<Decimal>>;
    std::vector<Series> closes(observations.closes.begin(), observations.closes.end());
    // Price momentum and volatility read the closes; the term structure the
    // carry of each observation, averaged over the window.
    const bool carry = input.factor() == protocol::proto::TERM_STRUCTURE;
    std::vector<Series> values(observations.terms.begin(), observations.terms.end());
    if (!carry)
        values = closes;
    const Feature feature = carry                       ? average
        : input.factor() == protocol::proto::VOLATILITY ? volatility
                                                        : momentum;
    const std::vector<unsigned> lookbacks(input.lookbacks().begin(), input.lookbacks().end());
    const bool search = lookbacks.size() > 1;
    factor::proto::FactorResult result;
    result.set_dataset_revision(input.dataset_revision());
    result.set_horizon(input.horizon());
    result.set_input_count(static_cast<unsigned>(observations.order.size()));
    result.set_evaluation_warmup(lookbacks.back());
    std::vector<std::vector<std::uint8_t>> masks;
    std::vector<std::span<const std::uint8_t>> eligibility;
    if (input.has_liquidity()) {
        const auto raw = protocol::factor_daily_observations(input);
        masks.resize(input.series_size(), std::vector<std::uint8_t>(observations.order.size()));
        const auto& rule = input.liquidity();
        for (int s = 0; s < input.series_size(); ++s) {
            std::vector<LiquidityObservation> facts;
            facts.reserve(raw.order.size());
            for (std::size_t i = 0; i < raw.order.size(); ++i)
                facts.push_back(
                    {raw.closes[s][i].has_value(), raw.volumes[s][i], raw.open_interests[s][i]});
            const auto reasons = liquidity_membership(
                facts,
                {rule.minimum_history(), rule.minimum_volume(), rule.minimum_open_interest()});
            for (std::size_t i = 0; i < raw.order.size(); ++i) {
                if (!raw.closes[s][i])
                    continue;
                auto* row = result.add_memberships();
                row->set_series_index(s);
                row->set_trading_day(
                    format_trading_date(std::chrono::sys_days{std::chrono::days{raw.order[i]}}));
                row->set_reason(
                    static_cast<factor::proto::MembershipReason>(static_cast<int>(reasons[i]) + 1));
                const auto found = std::ranges::lower_bound(observations.order, raw.order[i]);
                if (found != observations.order.end() && *found == raw.order[i])
                    masks[s][static_cast<std::size_t>(found - observations.order.begin())] =
                        reasons[i] == UniverseReason::eligible;
            }
        }
        eligibility.assign(masks.begin(), masks.end());
    }
    // Rows from `first` on, as observations of the whole input.
    const auto rows =
        [&](const FactorEvaluation& evaluation, std::size_t first, std::size_t offset) {
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
                for (const auto& member : row.members) {
                    auto* entry = target->add_members();
                    entry->set_series_index(static_cast<unsigned>(member.series_index));
                    entry->set_value(member.value);
                    entry->set_forward_return(member.forward_return);
                }
                if (row.pearson)
                    target->set_pearson(*row.pearson);
                if (row.spearman)
                    target->set_spearman(*row.spearman);
            }
        };
    if (!input.has_walk_forward()) {
        const auto evaluation = evaluate_factor(
            closes,
            values,
            feature,
            lookbacks,
            input.horizon(),
            input.has_holdout_start() ? std::optional<std::size_t>(input.holdout_start())
                                      : std::nullopt,
            stop,
            progress,
            eligibility);
        result.set_selection_rule(search ? "development_abs_spearman" : "fixed");
        result.set_lookback(evaluation.lookback);
        fill(*result.mutable_candidates(), evaluation.candidates);
        const bool holdout = evaluation.partitions.size() == 2;
        fill(
            *result.add_partitions(),
            evaluation.partitions[0],
            holdout ? "development" : "full_sample",
            0);
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
             observations.order,
             lookbacks.back(),
             input.horizon(),
             input.walk_forward().training_events(),
             input.walk_forward().validation_events())) {
        const auto training = range.training_end - range.training_begin;
        std::size_t units = 0;
        const auto within = [&](std::vector<Series> all) {
            for (auto& series : all)
                series = series.subspan(
                    range.training_begin,
                    range.validation_end - range.training_begin);
            return all;
        };
        auto fold_eligibility = eligibility;
        for (auto& mask : fold_eligibility)
            mask = mask.subspan(range.training_begin, range.validation_end - range.training_begin);
        const auto evaluation = evaluate_factor(
            within(closes),
            within(values),
            feature,
            lookbacks,
            input.horizon(),
            training,
            stop,
            [&](std::size_t done, std::size_t all) {
                units = all;
                if (progress)
                    progress(completed + done, total);
            },
            fold_eligibility);
        completed += units;
        auto* fold = result.add_folds();
        fold->set_training_begin(range.training_begin);
        fold->set_training_end(range.training_end);
        fold->set_validation_end(range.validation_end);
        fold->set_lookback(evaluation.lookback);
        fill(*fold->mutable_candidates(), evaluation.candidates);
        fill(
            *fold->mutable_development(),
            evaluation.partitions[0],
            "development",
            range.training_begin);
        fill(*fold->mutable_holdout(), evaluation.partitions[1], "holdout", range.training_begin);
        result.set_purged_count(result.purged_count() + static_cast<unsigned>(evaluation.purged));
        rows(evaluation, training, range.training_begin);
    }
    return result;
}

} // namespace asterion::factor
