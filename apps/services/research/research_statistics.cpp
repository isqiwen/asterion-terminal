#include "research_statistics.hpp"
#include "descriptive_statistics.hpp"
#include "task_payload.hpp"
#include "program_files.hpp"
#include <asterion/protocol/research_statistics.hpp>
#include <asterion/protocol/research_sample.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/domain/trading_days.hpp>
#include <asterion/foundation/error.hpp>
#include <chrono>

namespace asterion::tasks {
namespace {
namespace r = research::proto;

void statistic(r::StatisticValue& target, const std::optional<double>& value, StatisticState state)
{
    switch (state) {
        case StatisticState::defined:
            target.set_state(r::StatisticValue::DEFINED);
            break;
        case StatisticState::no_valid_samples:
            target.set_state(r::StatisticValue::NO_VALID_SAMPLES);
            break;
        case StatisticState::insufficient_samples:
            target.set_state(r::StatisticValue::INSUFFICIENT_SAMPLES);
            break;
        case StatisticState::non_finite:
            target.set_state(r::StatisticValue::NON_FINITE);
            break;
    }
    if (value)
        target.set_value(*value);
}

void fill_summary(r::StatisticsSummary& summary, const Distribution& values)
{
    summary.set_total(values.rows.size());
    summary.set_allowed(values.allowed);
    summary.set_valid(values.valid);
    summary.set_missing(values.allowed - values.valid);
    statistic(*summary.mutable_mean(), values.mean, values.mean_state);
    statistic(
        *summary.mutable_standard_deviation(),
        values.standard_deviation,
        values.standard_deviation_state);
    const auto quantile_state =
        values.valid ? StatisticState::defined : StatisticState::no_valid_samples;
    statistic(*summary.mutable_minimum(), values.minimum, quantile_state);
    statistic(*summary.mutable_q25(), values.q25, quantile_state);
    statistic(*summary.mutable_median(), values.median, quantile_state);
    statistic(*summary.mutable_q75(), values.q75, quantile_state);
    statistic(*summary.mutable_maximum(), values.maximum, quantile_state);
}

r::StatisticsRow::Reason reason(DistributionReason value)
{
    switch (value) {
        case DistributionReason::valid:
            return r::StatisticsRow::VALID;
        case DistributionReason::warmup:
            return r::StatisticsRow::WARMUP;
        case DistributionReason::zero_denominator:
            return r::StatisticsRow::ZERO_DENOMINATOR;
        case DistributionReason::non_finite:
            return r::StatisticsRow::NON_FINITE;
        case DistributionReason::unknown_label:
            return r::StatisticsRow::UNKNOWN_LABEL;
        case DistributionReason::crossed_label:
            return r::StatisticsRow::CROSSED_LABEL;
        case DistributionReason::other_role:
            return r::StatisticsRow::OTHER_ROLE;
    }
    throw std::logic_error("unknown distribution reason");
}

std::string observed(std::int64_t value, bool daily)
{
    using namespace std::chrono;
    if (daily)
        return format_trading_date(year_month_day{sys_days{days{value}}});
    return protocol::format_utc_nanoseconds(value);
}

} // namespace

research::proto::StatisticsInput capture_statistics_input(
    const resource::proto::Owner& owner,
    const research::proto::StatisticsRequest& request,
    const task::proto::TaskResponse& response,
    const std::string& input_digest,
    std::uint64_t input_bytes)
{
    protocol::validate_statistics_request(request);
    const auto& original = response.result_task();
    if (owner.SerializeAsString() != request.source().owner().SerializeAsString())
        throw Error(ErrorCode::conflict, "statistics source must belong to this Research service");
    const bool sample = request.source().kind() == "sample_result";
    if (!response.has_result_task()
        || (sample ? !response.has_sample() || !original.has_sample()
                    || original.kind() != task::proto::SAMPLE
                   : !response.has_factor() || !original.has_factor()
                    || original.kind() != task::proto::FACTOR)
        || original.state() != task::proto::SUCCEEDED || original.has_retirement()
        || original.id() != request.source().id()
        || original.result_digest() != request.source().version())
        throw Error(ErrorCode::conflict, "statistics source result is unavailable or changed");
    if (original.program_artifact().empty())
        throw Error(ErrorCode::unavailable, "statistics source program is unavailable");
    r::StatisticsInput input;
    *input.mutable_owner() = owner;
    *input.mutable_request() = request;
    auto& source = *input.mutable_source();
    *source.mutable_result() = request.source();
    source.set_input_sha256(input_digest);
    source.set_input_bytes(input_bytes);
    source.set_attempt(original.attempt());
    source.set_program_artifact(original.program_artifact());
    if (sample) {
        source.set_observations(static_cast<unsigned>(response.sample().rows_size()));
        source.set_price_power(1);
        if (original.sample().request().has_full_sample())
            source.set_full_sample(original.sample().request().full_sample());
        else
            source.set_holdout_start(original.sample().request().development_end());
        protocol::validate_statistics_source(input, original.sample(), response.sample());
        return input;
    }
    source.set_observations(response.factor().input_count());
    source.set_required_history(response.factor().evaluation_warmup());
    source.set_horizon(original.factor().horizon());
    source.set_price_power(
        original.factor().has_variable_definition()
            ? original.factor().variable_definition().price_power()
            : 0);
    if (original.factor().has_full_sample())
        source.set_full_sample(original.factor().full_sample());
    else if (original.factor().has_holdout_start())
        source.set_holdout_start(original.factor().holdout_start());
    protocol::validate_statistics_source(input, original.factor(), response.factor());
    return input;
}

research::proto::StatisticsResult compute_statistics(
    const research::proto::StatisticsInput& input,
    const std::filesystem::path& root,
    std::stop_token stop,
    const TaskProgress& progress)
{
    protocol::validate_statistics_input(input);
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "task cancelled");
    const auto& source = input.source();
    (void)programs::Files(root / "programs", input.owner()).find(source.program_artifact());
    const bool sample = source.result().kind() == "sample_result";
    factor::proto::FactorInput original;
    factor::proto::FactorResult evidence;
    r::SampleInput sample_input;
    r::SampleResult sample_evidence;
    const auto read_input = [&](google::protobuf::Message& target) {
        payload::parse(
            payload::load(
                root / source.result().id() / "input.pb",
                payload::max_input_bytes,
                source.input_bytes()),
            target);
    };
    const auto read_result = [&](google::protobuf::Message& target) {
        payload::parse(
            payload::load(
                root / source.result().id() / "results"
                    / (std::to_string(source.attempt()) + ".pb"),
                payload::max_result_bytes),
            target);
    };
    std::vector<DistributionObservation> rows;
    rows.reserve(source.observations());
    if (sample) {
        read_input(sample_input);
        read_result(sample_evidence);
        protocol::validate_statistics_source(input, sample_input, sample_evidence);
        for (const auto& entry : sample_evidence.rows()) {
            const auto role = entry.role() == r::SampleRow::FULL_SAMPLE
                ? DistributionRole::full_sample
                : entry.role() == r::SampleRow::DEVELOPMENT ? DistributionRole::development
                                                            : DistributionRole::holdout;
            // Decimal has eight fixed places. This conversion is solely a
            // statistical scalar; the exact source price remains in SampleInput.
            const double value = static_cast<double>(sample_input.dataset()
                                         .bars(static_cast<int>(entry.event_index()))
                                         .close()
                                         .units())
                / 100000000.0;
            rows.push_back({{value, FeatureUndefined::none}, std::nullopt, role, false});
        }
    } else {
        read_input(original);
        read_result(evidence);
        protocol::validate_statistics_source(input, original, evidence);
        for (const auto& entry : evidence.observations()) {
            FeatureUndefined state =
                entry.has_value() ? FeatureUndefined::none : FeatureUndefined::warmup;
            if (entry.has_feature_state()) {
                switch (entry.feature_state()) {
                    case factor::proto::FactorObservation::FEATURE_VALUE:
                        state = FeatureUndefined::none;
                        break;
                    case factor::proto::FactorObservation::FEATURE_WARMUP:
                        state = FeatureUndefined::warmup;
                        break;
                    case factor::proto::FactorObservation::FEATURE_ZERO_DENOMINATOR:
                        state = FeatureUndefined::zero_denominator;
                        break;
                    case factor::proto::FactorObservation::FEATURE_NON_FINITE:
                        state = FeatureUndefined::non_finite;
                        break;
                    default:
                        throw std::logic_error("invalid validated feature state");
                }
            }
            const auto role = entry.role() == factor::proto::FactorObservation::FULL_SAMPLE
                ? DistributionRole::full_sample
                : entry.role() == factor::proto::FactorObservation::DEVELOPMENT
                ? DistributionRole::development
                : DistributionRole::holdout;
            rows.push_back(
                {{entry.has_value() ? std::optional<double>{entry.value()} : std::nullopt, state},
                    entry.has_forward_return() ? std::optional<double>{entry.forward_return()}
                                               : std::nullopt,
                    role,
                    entry.label_crosses_boundary()});
        }
    }
    const auto& request = input.request();
    const DistributionSelection selection{
        request.column() == r::StatisticsRequest::FORWARD_RETURN
            ? DistributionMetric::forward_return
            : DistributionMetric::feature,
        request.role() == r::StatisticsRequest::FULL_SAMPLE ? DistributionRole::full_sample
                                                            : DistributionRole::development,
        request.begin_index(),
        request.end_index(),
        request.bins()
    };
    Distribution values;
    std::vector<DistributionGroup> groups;
    if (request.grouping() == r::StatisticsRequest::TRADING_DAY) {
        std::vector<std::string> days;
        days.reserve(sample_evidence.rows_size());
        for (const auto& row : sample_evidence.rows())
            days.push_back(row.trading_day());
        auto calculated = describe_distribution_groups(rows, days, selection, stop, progress);
        values = std::move(calculated.whole);
        groups = std::move(calculated.groups);
    } else
        values = describe_distribution(rows, selection, stop, progress);
    r::StatisticsResult result;
    *result.mutable_owner() = input.owner();
    *result.mutable_source() = request.source();
    result.set_price_power(
        request.column() == r::StatisticsRequest::FORWARD_RETURN ? 0 : source.price_power());
    fill_summary(*result.mutable_summary(), values);
    for (std::size_t i = 0; i < values.reasons.size(); ++i) {
        auto* item = result.add_reasons();
        item->set_reason(reason(static_cast<DistributionReason>(i)));
        item->set_count(values.reasons[i]);
    }
    for (const auto& bin : values.bins) {
        auto* item = result.add_bins();
        item->set_lower(bin.lower);
        item->set_upper(bin.upper);
        item->set_count(bin.count);
    }
    std::vector<std::int64_t> timeline;
    bool daily = false;
    if (!sample) {
        timeline = protocol::factor_observations(original).order;
        daily = original.series(0).has_table();
    }
    for (const auto& row : values.rows) {
        auto* item = result.add_rows();
        item->set_event_index(row.index);
        if (sample) {
            const auto& original_row = sample_evidence.rows(static_cast<int>(row.index));
            item->set_trading_day(original_row.trading_day());
            item->set_observed(
                sample_input.dataset().interval_minutes() == 0
                    ? original_row.trading_day()
                    : protocol::format_utc_nanoseconds(original_row.observed_ns()));
        } else
            item->set_observed(observed(timeline[row.index], daily));
        item->set_reason(reason(row.reason));
        if (row.value)
            item->set_value(*row.value);
        if (row.bin)
            item->set_bin(*row.bin);
    }
    for (const auto& group : groups) {
        auto* item = result.add_groups();
        item->set_trading_day(group.key);
        fill_summary(*item->mutable_summary(), group.distribution);
        for (std::size_t i = 0; i < group.distribution.reasons.size(); ++i) {
            auto* count = item->add_reasons();
            count->set_reason(reason(static_cast<DistributionReason>(i)));
            count->set_count(group.distribution.reasons[i]);
        }
        for (const auto& bin : group.distribution.bins)
            item->add_bin_counts(bin.count);
    }
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "task cancelled");
    protocol::validate_statistics_result(input, result);
    return result;
}

} // namespace asterion::tasks
