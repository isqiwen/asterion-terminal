#pragma once
#include <asterion/protocol/factor.hpp>
#include <functional>
#include <stop_token>

namespace asterion::factor {

factor::proto::FactorResult run(
    const factor::proto::FactorInput& input,
    std::stop_token stop = {},
    const std::function<void(std::size_t, std::size_t)>& progress = {});

} // namespace asterion::factor
