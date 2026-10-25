#include "model_strategy.hpp"
#include "strategy.hpp"
#include <asterion/protocol/research_model.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/time.hpp>
#include <algorithm>

namespace asterion::backtest {
namespace {
namespace r = research::proto;

class ModelStrategy final : public Strategy
{
public:
    ModelStrategy(
        const r::ModelReplayDefinition& definition,
        const ModelReplayData& data,
        r::ModelReplayResult& report,
        std::stop_token stop,
        std::function<void()> fitted)
        : Strategy(
              Decimal::from_raw(definition.quantity().units()),
              protocol::position_sides(definition.sides())),
          definition_(definition),
          data_(data),
          report_(report),
          stop_(stop),
          on_fit_(std::move(fitted))
    {
        protocol::validate_model_replay_definition(definition);
        if (data.features.size() != scope().validation_end()
            || data.training.size() != data.training_members.size()
            || data.training.size()
                != definition.training().plan().training_counts(
                    static_cast<int>(definition.fold_index())))
            throw std::invalid_argument("model replay data must be the exact original prefix");
        check();
    }

private:
    const r::ModelTimeFold& scope() const
    {
        return definition_.training().plan().specification().folds(
            static_cast<int>(definition_.fold_index()));
    }

    void check() const
    {
        if (stop_.stop_requested())
            throw Error(ErrorCode::cancelled, "model replay cancelled");
        if (std::chrono::steady_clock::now() >= data_.deadline)
            throw Error(ErrorCode::resource_exhausted, "model replay time budget exhausted");
        if (SystemClock{}.utc_now() / 1000000
            >= definition_.mapping().specification().valid_until_ms())
            throw Error(ErrorCode::conflict, "model replay mapping expired before completion");
    }

    void reset() override
    {
        index_ = 0;
        fitted_.reset();
        scale_.reset();
        report_.Clear();
        report_.set_historical_availability_unknown(true);
    }

    void fit(const MarketBar& boundary)
    {
        const auto started = std::chrono::steady_clock::now();
        const auto started_ms = SystemClock{}.utc_now() / 1000000;
        const auto& plan = definition_.training().plan();
        const auto& recipe = plan.recipe().specification();
        const auto deadline = std::min(
            data_.deadline,
            started + std::chrono::seconds(plan.specification().time_budget_seconds()));
        const auto check_fit = [&] {
            check();
            if (std::chrono::steady_clock::now() >= deadline)
                throw Error(
                    ErrorCode::resource_exhausted,
                    "model replay fit time budget exhausted");
        };
        // The Research owner projected only the original mature training members.
        // Their values are first fitted here, when the replay reaches the declared cut.
        std::vector<models::RegressionPoint> points = data_.training;
        const auto& members = data_.training_members;
        const auto progress = [&](auto, auto) {
            check_fit();
        };
        if (recipe.standardize()) {
            scale_ = models::fit_standardization(points, stop_, progress, deadline);
            for (auto& point : points) {
                check_fit();
                point.feature = models::standardize_feature(*scale_, point.feature);
            }
        }
        models::RegressionOptions options{
            recipe.method() == r::MODEL_LINEAR ? models::RegressionMethod::linear
                                               : models::RegressionMethod::ridge,
            recipe.fit_intercept(),
            {}
        };
        if (recipe.has_alpha())
            options.alpha = static_cast<double>(recipe.alpha().units()) / 100000000.0;
        fitted_ = models::fit_regression(points, options, stop_, progress, deadline);
        check_fit();
        auto& fit = *report_.mutable_fit();
        for (const auto member : members)
            fit.add_training_members(member);
        fit.set_intercept(fitted_->intercept);
        fit.set_coefficient(fitted_->coefficient);
        fit.set_training_residual_sum_squares(fitted_->residual_sum_squares);
        fit.set_regularization(fitted_->regularization);
        fit.set_training_objective(fitted_->objective);
        if (scale_) {
            auto& scale = *fit.mutable_standardization();
            scale.set_training_samples(static_cast<unsigned>(scale_->samples));
            scale.set_mean(scale_->mean);
            scale.set_sample_standard_deviation(scale_->sample_standard_deviation);
        }
        fit.set_started_index(scope().training_end());
        fit.set_ready_index(scope().training_end() + definition_.processing_delay_bars());
        fit.set_started_boundary_ns(boundary.timestamp_ns);
        fit.set_actual_started_at_ms(started_ms);
        fit.set_actual_fitted_at_ms(SystemClock{}.utc_now() / 1000000);
        fit.set_actual_duration_ns(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started)
                .count());
        if (on_fit_)
            on_fit_();
    }

    std::optional<int> decide(const MarketBar& bar) override
    {
        check();
        if (index_ >= data_.features.size())
            throw std::invalid_argument("model replay received a bar outside its fixed prefix");
        const auto index = index_++;
        const auto& feature_value = data_.features[index];
        auto& row = *report_.add_decisions();
        row.set_source_index(static_cast<unsigned>(index));
        row.set_boundary_ns(bar.timestamp_ns);
        if (feature_value)
            row.set_feature(*feature_value);
        row.set_intent(r::ModelMappedRow::PREDICTION_UNAVAILABLE);
        row.set_disposition(r::ModelReplayDecision::NO_NEW_TARGET);
        if (index < scope().training_end()) {
            row.set_phase(r::ModelReplayDecision::BEFORE_TRAINING);
            return {};
        }
        if (index == scope().training_end())
            fit(bar);
        const auto ready = report_.fit().ready_index();
        if (index < ready) {
            row.set_phase(r::ModelReplayDecision::MODEL_PENDING);
            return {};
        }
        if (index == ready)
            report_.mutable_fit()->set_ready_boundary_ns(bar.timestamp_ns);
        if (index < scope().validation_begin() || index >= scope().validation_end()) {
            row.set_phase(r::ModelReplayDecision::OUTSIDE_VALIDATION);
            return {};
        }
        if (bar.timestamp_ns >= definition_.mapping().specification().valid_until_ms() * 1000000) {
            row.set_phase(r::ModelReplayDecision::EXPIRED);
            row.set_intent(r::ModelMappedRow::EXPIRED);
            return {};
        }
        if (!feature_value) {
            row.set_phase(r::ModelReplayDecision::FEATURE_UNAVAILABLE);
            return {};
        }
        std::optional<double> prediction;
        try {
            const auto feature =
                scale_ ? models::standardize_feature(*scale_, *feature_value) : *feature_value;
            prediction = models::predict_regression(*fitted_, feature);
        } catch (const std::invalid_argument&) {
            // Source/fit validation is outside this numeric-only overflow boundary.
            row.set_phase(r::ModelReplayDecision::NON_FINITE_PREDICTION);
            return {};
        }
        row.set_prediction(*prediction);
        row.set_phase(r::ModelReplayDecision::MAPPED);
        const models::ThresholdMapping thresholds{
            static_cast<double>(definition_.mapping().specification().short_threshold().units())
                / 100000000.0,
            static_cast<double>(definition_.mapping().specification().long_threshold().units())
                / 100000000.0
        };
        switch (models::map_prediction(thresholds, prediction)) {
            case models::PredictionIntent::long_direction:
                row.set_intent(r::ModelMappedRow::LONG_DIRECTION);
                return 1;
            case models::PredictionIntent::short_direction:
                row.set_intent(r::ModelMappedRow::SHORT_DIRECTION);
                return -1;
            case models::PredictionIntent::no_new_direction:
                row.set_intent(r::ModelMappedRow::NO_NEW_DIRECTION);
                return {};
            case models::PredictionIntent::unavailable:
                throw Error(
                    ErrorCode::internal_error,
                    "finite model replay prediction became unavailable");
        }
        throw Error(ErrorCode::internal_error, "unknown model replay prediction intent");
    }

    const r::ModelReplayDefinition& definition_;
    const ModelReplayData& data_;
    r::ModelReplayResult& report_;
    std::stop_token stop_;
    std::function<void()> on_fit_;
    std::size_t index_ = 0;
    std::optional<models::RegressionFit> fitted_;
    std::optional<models::Standardization> scale_;
};

} // namespace

std::unique_ptr<Strategy> make_model_strategy(
    const research::proto::ModelReplayDefinition& definition,
    const ModelReplayData& data,
    research::proto::ModelReplayResult& report,
    std::stop_token stop,
    const std::function<void()>& fitted)
{
    return std::make_unique<ModelStrategy>(definition, data, report, stop, fitted);
}

} // namespace asterion::backtest
