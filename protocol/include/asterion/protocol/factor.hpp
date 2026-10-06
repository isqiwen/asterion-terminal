#pragma once
#include <asterion/protocol/trading.hpp>
#include <asterion/domain/daily_bars.hpp>
#include <asterion/v1/factor.pb.h>
namespace asterion::protocol {
factor::v1::DailyFactorRequest encode_daily_factor_request(const Json&);
Json decode_daily_factor(const factor::v1::DailyFactorInput&, const factor::v1::DailyFactorResult&);
std::vector<HistoricalDailyBar> daily_factor_bars(const factor::v1::DailyFactorDataset&);
std::string daily_factor_revision(const factor::v1::DailyFactorDataset&);
void validate_daily_factor(const factor::v1::DailyFactorInput&);
factor::v1::FactorRequest encode_factor_request(const Json& input);
void validate_factor_input(const factor::v1::FactorInput& input);
// Input is validated before admission. Progress units are part of the task contract.
std::size_t factor_work_units(const factor::v1::FactorInput& input);
// Validate identity, timeline and evaluation boundaries without executing the model.
void validate_factor_result(const factor::v1::FactorInput& input,
                            const factor::v1::FactorResult& result);
Json decode_factor(const factor::v1::FactorInput& input, DatasetView view = DatasetView::full);
Json decode_factor_result(const factor::v1::FactorResult& result);
} // namespace asterion::protocol
