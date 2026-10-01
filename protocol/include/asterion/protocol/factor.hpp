#pragma once
#include <asterion/protocol/trading.hpp>
#include <asterion/domain/daily_bars.hpp>
#include <asterion/v1/factor.pb.h>
namespace asterion::protocol {
research::v1::DailyFactorRequest encode_daily_factor_request(const Json&);
Json decode_daily_factor(const research::v1::DailyFactorInput&,
                         const research::v1::DailyFactorResult&);
std::vector<HistoricalDailyBar> daily_factor_bars(const research::v1::DailyFactorDataset&);
std::string daily_factor_revision(const research::v1::DailyFactorDataset&);
void validate_daily_factor(const research::v1::DailyFactorInput&);
std::string factor_dataset_revision(const research::v1::FactorInput& input);
research::v1::FactorInput encode_factor(const Json& input);
research::v1::FactorRequest encode_factor_request(const Json& input);
Json decode_factor(const research::v1::FactorInput& input);
Json decode_factor_result(const research::v1::FactorResult& result);
} // namespace asterion::protocol
