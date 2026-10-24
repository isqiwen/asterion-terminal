#pragma once
#include <optional>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/data_service.pb.h>
#include <asterion/factor.pb.h>

namespace asterion::protocol {

// A cross-section compares at most this many series: every product of the
// market-wide table at once.
inline constexpr int max_factor_series = max_table_instruments;
factor::proto::FactorRequest encode_factor_request(const Json& input);
// Reuses the parameters of a currently decoded daily experiment. Empty snapshot
// keeps each original snapshot; a supplied snapshot replaces them explicitly.
// Derive from the fixed input's execution parameters, without presentation JSON.
factor::proto::FactorRequest reuse_daily_factor(
    factor::proto::FactorRequest parameters,
    const std::string& dataset_revision,
    const std::string& snapshot,
    const std::optional<std::vector<unsigned>>& lookbacks = {});
factor::proto::FactorRequest reuse_daily_factor(
    const Json& experiment,
    const std::string& snapshot,
    const Json& lookbacks = nullptr);
// What Data is asked for one entry of a request, and the fixed series its
// reply makes: one, or one for each instrument of the market-wide table.
data::proto::DataRequest factor_series_query(const factor::proto::FactorSeriesRequest& source);
std::vector<factor::proto::FactorSeries>
factor_series(const factor::proto::FactorSeriesRequest& source, data::proto::DataResponse reply);
// The fixed input of a request, before its series: Task and the worker each
// resolve them from Data, add them in request order and then set the revision.
factor::proto::FactorInput factor_input(const factor::proto::FactorRequest& request);
// Refuses more observations than one input may hold.
void add_factor_series(factor::proto::FactorInput& input, factor::proto::FactorSeries series);
// The revision of an input's data: of its only series, or of all of them in order.
std::string factor_revision(
    const google::protobuf::RepeatedPtrField<factor::proto::FactorSeries>& series);

// What a factor reads from the series of a valid input: every observation any
// of them has, in order. `closes` holds for each series its positive close at
// each observation, or nothing where the series was not observed; `order` says
// when, as a number that increases. `terms` holds each observation's carry for
// the term structure and is empty for the factors that read the closes.
struct FactorObservations
{
    std::vector<std::vector<std::optional<Decimal>>> closes, terms;
    std::vector<std::int64_t> order;
    // Table daily facts only; empty for bar inputs. Missing source values stay absent.
    std::vector<std::vector<std::optional<std::int64_t>>> volumes, open_interests;
};

FactorObservations factor_observations(const factor::proto::FactorInput& input);

// Exact single-series source labels in factor observation order. Daily tables have
// a provider trading date only; their ordering ordinal is not a UTC timestamp.
struct FactorObservationClock
{
    std::string trading_day;
    std::optional<std::int64_t> timestamp_ns;
};

std::vector<FactorObservationClock> factor_observation_clocks(
    const factor::proto::FactorInput& input);
// Raw daily timeline, independent of factor type and forward labels.
FactorObservations factor_daily_observations(const factor::proto::FactorInput& input);
// Validation also produces the aligned observations, owned by the caller. Workers
// use this result directly instead of preparing the same input again.
FactorObservations validate_factor_input(const factor::proto::FactorInput& input);
// Input is validated before admission. Progress units are part of the task contract.
std::size_t factor_work_units(const factor::proto::FactorInput& input);
// Validate identity, sample indices and evaluation boundaries without executing the model.
void validate_factor_result(
    const factor::proto::FactorInput& input,
    const factor::proto::FactorResult& result);
Json decode_factor(const factor::proto::FactorInput& input, DatasetView view = DatasetView::full);
// Samples are shown with the times the input records for them.
Json decode_factor_result(
    const factor::proto::FactorInput& input,
    const factor::proto::FactorResult& result);

} // namespace asterion::protocol
