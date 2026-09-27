#pragma once
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/factor.pb.h>
namespace asterion::protocol {
std::string factor_dataset_revision(const research::v1::FactorInput &input);
research::v1::FactorInput encode_factor(const Json &input);
Json decode_factor(const research::v1::FactorInput &input);
Json decode_factor_result(const research::v1::FactorResult &result);
} // namespace asterion::protocol
