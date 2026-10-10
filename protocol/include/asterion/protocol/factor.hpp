#pragma once
#include <asterion/protocol/trading.hpp>
#include <asterion/domain/daily_bars.hpp>
#include <asterion/v1/data_service.pb.h>
#include <asterion/v1/factor.pb.h>
namespace asterion::protocol {
// Identifies how features, labels, partitions and selection are computed.
inline constexpr char factor_engine_version[] = "asterion.factor.momentum.v6";
std::vector<HistoricalDailyBar> daily_factor_bars(const factor::v1::DailyFactorDataset&);
std::string daily_factor_revision(const factor::v1::DailyFactorDataset&);
factor::v1::FactorRequest encode_factor_request(const Json& input);
// What Data is asked for one requested series, and the fixed series its reply makes.
data::v1::DataRequest factor_series_query(const factor::v1::FactorSeriesRequest& source);
factor::v1::FactorSeries factor_series(const factor::v1::FactorSeriesRequest& source,
                                       data::v1::DataResponse reply);
std::string factor_series_revision(const factor::v1::FactorSeries& series);
// What a factor reads from the series of a valid input, in order: one positive
// close per observation, and when it was observed as a number that never
// decreases along the series.
struct FactorObservations {
  std::vector<Decimal> closes;
  std::vector<std::int64_t> order;
};
FactorObservations factor_observations(const factor::v1::FactorInput& input);
void validate_factor_input(const factor::v1::FactorInput& input);
// Input is validated before admission. Progress units are part of the task contract.
std::size_t factor_work_units(const factor::v1::FactorInput& input);
// Validate identity, sample indices and evaluation boundaries without executing the model.
void validate_factor_result(const factor::v1::FactorInput& input,
                            const factor::v1::FactorResult& result);
Json decode_factor(const factor::v1::FactorInput& input, DatasetView view = DatasetView::full);
// Samples are shown with the times the input records for them.
Json decode_factor_result(const factor::v1::FactorInput& input,
                          const factor::v1::FactorResult& result);
} // namespace asterion::protocol
