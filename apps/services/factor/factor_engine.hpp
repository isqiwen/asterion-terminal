#pragma once
#include <asterion/protocol/factor.hpp>
#include <functional>
#include <stop_token>
namespace asterion::factor {
research::v1::DailyFactorResult
run_daily(const research::v1::DailyFactorInput&, std::stop_token stop = {},
          const std::function<void(std::size_t, std::size_t)>& progress = {});
void verify_daily_result(const research::v1::DailyFactorInput&,
                         const research::v1::DailyFactorResult&);
void verify_result(const research::v1::FactorInput& input,
                   const research::v1::FactorResult& result);
std::size_t work_units(const research::v1::FactorInput& input);
void validate(const research::v1::FactorInput& input);
research::v1::FactorResult run(const research::v1::FactorInput& input, std::stop_token stop = {},
                               const std::function<void(std::size_t, std::size_t)>& progress = {});
} // namespace asterion::factor
