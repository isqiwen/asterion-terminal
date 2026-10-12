#pragma once
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/domain/daily_bars.hpp>
#include <asterion/v1/data_service.pb.h>
#include <asterion/v1/factor.pb.h>
namespace asterion::protocol {
// Identifies how features, labels, partitions and selection are computed.
inline constexpr char factor_engine_version[] = "asterion.factor.v9";
// A cross-section compares at most this many series: every product of the
// market-wide table at once.
inline constexpr int max_factor_series = max_table_instruments;
std::vector<HistoricalDailyBar> daily_factor_bars(const factor::v1::DailyFactorDataset&);
std::string daily_factor_revision(const factor::v1::DailyFactorDataset&);
factor::v1::FactorRequest encode_factor_request(const Json& input);
// What Data is asked for one entry of a request, and the fixed series its
// reply makes: one, or one for each instrument of the market-wide table.
data::v1::DataRequest factor_series_query(const factor::v1::FactorSeriesRequest& source);
std::vector<factor::v1::FactorSeries> factor_series(const factor::v1::FactorSeriesRequest& source,
                                                    data::v1::DataResponse reply);
// The fixed input of a request, before its series: Task and the worker each
// resolve them from Data, add them in request order and then set the revision.
factor::v1::FactorInput factor_input(const factor::v1::FactorRequest& request);
// Refuses more observations than one input may hold.
void add_factor_series(factor::v1::FactorInput& input, factor::v1::FactorSeries series);
// The revision of an input's data: of its only series, or of all of them in order.
std::string
factor_revision(const google::protobuf::RepeatedPtrField<factor::v1::FactorSeries>& series);
// What a factor reads from the series of a valid input: every observation any
// of them has, in order. `closes` holds for each series its positive close at
// each observation, or nothing where the series was not observed; `order` says
// when, as a number that increases. `terms` holds each observation's carry for
// the term structure and is empty for the factors that read the closes.
struct FactorObservations {
  std::vector<std::vector<std::optional<Decimal>>> closes, terms;
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
