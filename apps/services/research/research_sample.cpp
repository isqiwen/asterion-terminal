#include "research_sample.hpp"
#include "descriptive_statistics.hpp"
#include <asterion/protocol/research_sample.hpp>
#include <asterion/foundation/error.hpp>

namespace asterion::tasks {

research::proto::SampleResult capture_sample(
    const research::proto::SampleInput& input,
    std::stop_token stop,
    const TaskProgress& progress)
{
    protocol::validate_sample_input(input);
    const auto count = static_cast<std::size_t>(input.dataset().bars_size());
    const auto roles = observation_roles(
        count,
        input.request().has_development_end()
            ? std::optional<std::size_t>(input.request().development_end())
            : std::nullopt,
        stop,
        [&](std::size_t completed, std::size_t) {
            if (progress)
                progress(completed, 2 * count);
        });
    research::proto::SampleResult result;
    *result.mutable_owner() = input.owner();
    result.set_dataset_revision(input.dataset().revision());
    result.mutable_rows()->Reserve(static_cast<int>(count));
    for (std::size_t i = 0; i < count; ++i) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "task cancelled");
        const auto& source = input.dataset().bars(static_cast<int>(i));
        auto& row = *result.add_rows();
        row.set_event_index(static_cast<unsigned>(i));
        row.set_observed_ns(source.timestamp_ns());
        row.set_trading_day(source.trading_day());
        switch (roles[i]) {
            case DistributionRole::full_sample:
                row.set_role(research::proto::SampleRow::FULL_SAMPLE);
                break;
            case DistributionRole::development:
                row.set_role(research::proto::SampleRow::DEVELOPMENT);
                break;
            case DistributionRole::holdout:
                row.set_role(research::proto::SampleRow::HOLDOUT);
                break;
        }
        if (progress && (i + 1) % 4096 == 0)
            progress(count + i + 1, 2 * count);
    }
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "task cancelled");
    protocol::validate_sample_result(input, result);
    if (progress)
        progress(2 * count, 2 * count);
    return result;
}

} // namespace asterion::tasks
