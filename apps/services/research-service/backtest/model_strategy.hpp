#pragma once
#include "model_regression.hpp"
#include <asterion/research_model.pb.h>
#include <asterion/data.pb.h>
#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>

namespace asterion {

class Strategy;

} // namespace asterion

namespace asterion::backtest {

// Research resolves this projection from verified fixed source files. Only the
// replay prefix is retained; later validation/holdout labels are not fitting inputs.
struct ModelReplayData
{
    std::vector<std::optional<double>> features;
    std::vector<models::RegressionPoint> training;
    std::vector<std::uint32_t> training_members;
    std::string plan_digest;
    std::uint32_t fold_index = 0;
    std::string source_revision;
    data::proto::DataOwner data_owner;
    std::chrono::steady_clock::time_point deadline;
};

std::unique_ptr<Strategy> make_model_strategy(
    const research::proto::ModelReplayDefinition&,
    const ModelReplayData&,
    research::proto::ModelReplayResult&,
    std::stop_token,
    const std::function<void()>& fitted);

} // namespace asterion::backtest
