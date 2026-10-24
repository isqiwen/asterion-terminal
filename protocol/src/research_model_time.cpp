#include <asterion/protocol/research_model.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/domain/trading_days.hpp>
#include <array>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace asterion::protocol {
namespace {
namespace r = research::proto;

bool content(const std::string& value, std::size_t limit)
{
    return !value.empty() && value.size() <= limit && value.find(char(0)) == std::string::npos
        && !std::ranges::all_of(value, [](unsigned char c) {
               return std::isspace(c);
           });
}

Json fold_json(const r::ModelTimeFold& fold)
{
    return {
        {"training_begin", fold.training_begin()},
        {"training_end", fold.training_end()},
        {"validation_begin", fold.validation_begin()},
        {"validation_end", fold.validation_end()}
    };
}

} // namespace

void validate_model_time_specification(const r::ModelTimeSpecification& value)
{
    validate_message(value);
    if (!content(value.name(), 320) || !content(value.purpose(), 4096)
        || !content(value.limitations(), 4096) || value.folds_size() < 1 || value.folds_size() > 32
        || value.run_budget() < static_cast<unsigned>(value.folds_size())
        || !value.time_budget_seconds() || value.valid_until_ms() <= 0
        || value.valid_until_ms() > 9223372036854LL || value.ByteSizeLong() > 16384)
        throw std::invalid_argument(
            "model time validation requires explicit bounded folds and budgets");
    validate_strategy_object(value.recipe(), "model_plan");
    unsigned previous_end = 0;
    for (const auto& fold : value.folds()) {
        if (fold.training_begin() >= fold.training_end()
            || fold.training_end() > fold.validation_begin()
            || fold.validation_begin() >= fold.validation_end() || fold.validation_end() > 200000
            || fold.validation_begin() < previous_end)
            throw std::invalid_argument(
                "model time folds must be chronological and disjoint in validation");
        previous_end = fold.validation_end();
    }
}

std::string model_time_plan_digest(r::ModelTimePlan value)
{
    value.clear_digest();
    value.clear_recorded_at_ms();
    return sha256_bytes(value.SerializeAsString());
}

void validate_model_time_plan(const r::ModelTimePlan& value)
{
    validate_message(value);
    validate_id(value.id());
    validate_model_time_specification(value.specification());
    validate_model_plan(value.recipe());
    const auto& spec = value.specification();
    const auto& base = value.recipe();
    if (value.owner().SerializeAsString() != base.owner().SerializeAsString()
        || model_plan_object(base).SerializeAsString() != spec.recipe().SerializeAsString()
        || value.recorded_at_ms() <= 0 || value.recorded_at_ms() >= spec.valid_until_ms()
        || spec.valid_until_ms() > base.specification().valid_until_ms()
        || value.training_counts_size() != spec.folds_size()
        || value.boundaries_size() != spec.folds_size() * 4
        || value.digest() != model_time_plan_digest(value) || value.ByteSizeLong() > 65536
        || spec.folds(0).training_begin() != base.specification().training_begin()
        || spec.folds(0).training_end() != base.specification().training_end())
        throw std::invalid_argument("invalid immutable model time validation plan");
    for (int i = 0; i < spec.folds_size(); ++i) {
        const auto& fold = spec.folds(i);
        const auto count = value.training_counts(i);
        const std::array indices{
            fold.training_begin(),
            fold.training_end(),
            fold.validation_begin(),
            fold.validation_end()
        };
        for (int j = 0; j < 4; ++j) {
            const auto& boundary = value.boundaries(i * 4 + j);
            (void)parse_trading_date(boundary.trading_day());
            if (boundary.event_index() != indices[j]
                || boundary.has_timestamp_ns() != value.boundaries(0).has_timestamp_ns())
                throw std::invalid_argument("model time source boundary mismatch");
        }
        if (fold.validation_end() > base.source().holdout_start() || count < 2
            || count > fold.training_end() - fold.training_begin()
            || (i == 0 && count != base.training_samples()))
            throw std::invalid_argument(
                "model time plan includes holdout or inconsistent mature membership");
    }
}

resource::proto::Object model_time_plan_object(const r::ModelTimePlan& value)
{
    validate_model_time_plan(value);
    resource::proto::Object object;
    *object.mutable_owner() = value.owner();
    object.set_kind("model_time_plan");
    object.set_id(value.id());
    object.set_version(value.digest());
    return object;
}

void validate_model_time_input(const r::ModelTimeInput& value)
{
    validate_message(value);
    validate_model_time_plan(value.plan());
    if (value.ByteSizeLong() > 65536)
        throw std::invalid_argument("model time input exceeds metadata capacity");
}

void validate_model_time_result(const r::ModelTimeInput& input, const r::ModelTimeResult& value)
{
    validate_model_time_input(input);
    validate_message(value);
    const auto& plan = input.plan();
    const auto& recipe = plan.recipe().specification();
    if (value.plan().SerializeAsString() != model_time_plan_object(plan).SerializeAsString()
        || value.folds_size() != plan.specification().folds_size()
        || value.computed_at_ms() < plan.recorded_at_ms()
        || value.computed_at_ms() >= plan.specification().valid_until_ms())
        throw std::invalid_argument("model time result differs from its fixed plan or validity");
    for (int i = 0; i < value.folds_size(); ++i) {
        const auto& fit = value.folds(i);
        const auto& scope = plan.specification().folds(i);
        const auto count = plan.training_counts(i);
        const auto alpha =
            recipe.has_alpha() ? static_cast<double>(recipe.alpha().units()) / 100000000.0 : 0;
        const auto penalty = static_cast<double>(
            static_cast<long double>(alpha) * fit.coefficient() * fit.coefficient());
        if (fit.scope().SerializeAsString() != scope.SerializeAsString()
            || fit.training_members_size() != static_cast<int>(count)
            || !std::isfinite(fit.intercept()) || !std::isfinite(fit.coefficient())
            || (!recipe.fit_intercept() && fit.intercept() != 0)
            || !std::isfinite(fit.training_residual_sum_squares())
            || fit.training_residual_sum_squares() < 0 || !std::isfinite(fit.regularization())
            || fit.regularization() != penalty || !std::isfinite(fit.training_objective())
            || fit.training_objective()
                != fit.training_residual_sum_squares() + fit.regularization()
            || fit.has_standardization() != recipe.standardize()
            || static_cast<unsigned>(fit.predictions_size())
                != scope.validation_end() - scope.validation_begin()
            || !std::isfinite(fit.validation_residual_sum_squares())
            || fit.validation_residual_sum_squares() < 0)
            throw std::invalid_argument("invalid original model time fold fit");
        unsigned previous = 0;
        for (int j = 0; j < fit.training_members_size(); ++j) {
            const auto index = fit.training_members(j);
            if (index < scope.training_begin() || index >= scope.training_end()
                || (j && index <= previous))
                throw std::invalid_argument(
                    "model time training members differ from declared scope");
            previous = index;
        }
        if (fit.has_standardization()
            && (fit.standardization().training_samples() != count
                || !std::isfinite(fit.standardization().mean())
                || !std::isfinite(fit.standardization().sample_standard_deviation())
                || fit.standardization().sample_standard_deviation() <= 0))
            throw std::invalid_argument(
                "model time standardization must match its mature fold members");
        unsigned evaluated = 0;
        for (int j = 0; j < fit.predictions_size(); ++j) {
            const auto& row = fit.predictions(j);
            if (row.event_index() != scope.validation_begin() + static_cast<unsigned>(j)
                || row.exclusion() < r::ModelTimeRow::INCLUDED
                || row.exclusion() > r::ModelTimeRow::NON_FINITE_PREDICTION
                || row.has_prediction()
                    != (row.exclusion() != r::ModelTimeRow::FEATURE_UNAVAILABLE
                        && row.exclusion() != r::ModelTimeRow::NON_FINITE_PREDICTION)
                || (row.has_prediction() && !std::isfinite(row.prediction())))
                throw std::invalid_argument("invalid original model time prediction row");
            if (row.exclusion() == r::ModelTimeRow::INCLUDED)
                ++evaluated;
        }
        if (fit.evaluated() != evaluated
            || fit.has_validation_mean_squared_error() != (evaluated > 0)
            || (!evaluated && fit.validation_residual_sum_squares() != 0)
            || (evaluated
                && (!std::isfinite(fit.validation_mean_squared_error())
                    || fit.validation_mean_squared_error()
                        != fit.validation_residual_sum_squares() / evaluated)))
            throw std::invalid_argument(
                "model time validation score must use only eligible original rows");
    }
}

Json decode_model_time_specification(const r::ModelTimeSpecification& value)
{
    validate_model_time_specification(value);
    Json folds = Json::array();
    for (const auto& fold : value.folds())
        folds.push_back(fold_json(fold));
    return {
        {"name", value.name()},
        {"purpose", value.purpose()},
        {"limitations", value.limitations()},
        {"recipe", decode_strategy_object(value.recipe())},
        {"folds", std::move(folds)},
        {"run_budget", value.run_budget()},
        {"time_budget_seconds", value.time_budget_seconds()},
        {"valid_until_ms", value.valid_until_ms()}
    };
}

r::ModelTimeSpecification encode_model_time_specification(const Json& input)
{
    require_fields(
        input,
        {"name",
            "purpose",
            "limitations",
            "recipe",
            "folds",
            "run_budget",
            "time_budget_seconds",
            "valid_until_ms"});
    r::ModelTimeSpecification value;
    value.set_name(input.at("name").get<std::string>());
    value.set_purpose(input.at("purpose").get<std::string>());
    value.set_limitations(input.at("limitations").get<std::string>());
    *value.mutable_recipe() = encode_strategy_object(input.at("recipe"), "model_plan");
    const auto integer = [](const Json& object, const char* key, std::uint64_t limit) {
        const auto& number = object.at(key);
        if (!number.is_number_integer() || number < 0 || number > limit)
            throw std::invalid_argument("model time parameters require bounded integers");
        return number.get<unsigned>();
    };
    if (!input.at("folds").is_array() || input.at("folds").empty() || input.at("folds").size() > 32)
        throw std::invalid_argument(
            "model time validation requires one to thirty-two explicit folds");
    for (const auto& row : input.at("folds")) {
        require_fields(
            row,
            {"training_begin", "training_end", "validation_begin", "validation_end"});
        auto& fold = *value.add_folds();
        fold.set_training_begin(integer(row, "training_begin", 200000));
        fold.set_training_end(integer(row, "training_end", 200000));
        fold.set_validation_begin(integer(row, "validation_begin", 200000));
        fold.set_validation_end(integer(row, "validation_end", 200000));
    }
    value.set_run_budget(integer(input, "run_budget", 4294967295ULL));
    value.set_time_budget_seconds(integer(input, "time_budget_seconds", 4294967295ULL));
    if (!input.at("valid_until_ms").is_number_integer())
        throw std::invalid_argument("model time validity must be integer UTC milliseconds");
    value.set_valid_until_ms(input.at("valid_until_ms").get<std::int64_t>());
    validate_model_time_specification(value);
    return value;
}

Json decode_model_time_plan(const r::ModelTimePlan& value)
{
    validate_model_time_plan(value);
    Json counts = Json::array(), boundaries = Json::array();
    for (const auto& boundary : value.boundaries())
        boundaries.push_back(
            {{"event_index", boundary.event_index()},
                {"trading_day", boundary.trading_day()},
                {"timestamp_ns",
                    boundary.has_timestamp_ns() ? Json(std::to_string(boundary.timestamp_ns()))
                                                : Json(nullptr)}});
    for (const auto count : value.training_counts())
        counts.push_back(count);
    return {
        {"object", decode_strategy_object(model_time_plan_object(value))},
        {"specification", decode_model_time_specification(value.specification())},
        {"recipe", decode_model_plan(value.recipe())},
        {"training_counts", std::move(counts)},
        {"boundaries", std::move(boundaries)},
        {"recorded_at_ms", value.recorded_at_ms()},
        {"mode", "post_hoc_diagnostic"}
    };
}

Json decode_model_time_input(const r::ModelTimeInput& value)
{
    validate_model_time_input(value);
    return {{"plan", decode_model_time_plan(value.plan())}};
}

Json decode_model_time_result(const r::ModelTimeInput& input, const r::ModelTimeResult& value)
{
    validate_model_time_result(input, value);
    Json folds = Json::array();
    const char* exclusions[] = {
        "",
        "included",
        "warmup",
        "feature_unavailable",
        "label_unavailable",
        "crossed_label",
        "non_finite_prediction"
    };
    for (const auto& fit : value.folds()) {
        Json members = Json::array(), rows = Json::array();
        for (const auto index : fit.training_members())
            members.push_back(index);
        for (const auto& row : fit.predictions())
            rows.push_back(
                {{"event_index", row.event_index()},
                    {"prediction", row.has_prediction() ? Json(row.prediction()) : Json(nullptr)},
                    {"exclusion", exclusions[row.exclusion()]}});
        Json scale = nullptr;
        if (fit.has_standardization())
            scale = {
                {"training_samples", fit.standardization().training_samples()},
                {"mean", fit.standardization().mean()},
                {"sample_standard_deviation", fit.standardization().sample_standard_deviation()}
            };
        folds.push_back(
            {{"scope", fold_json(fit.scope())},
                {"training_members", std::move(members)},
                {"intercept", fit.intercept()},
                {"coefficient", fit.coefficient()},
                {"training_residual_sum_squares", fit.training_residual_sum_squares()},
                {"regularization", fit.regularization()},
                {"training_objective", fit.training_objective()},
                {"standardization", std::move(scale)},
                {"predictions", std::move(rows)},
                {"evaluated", fit.evaluated()},
                {"validation_residual_sum_squares", fit.validation_residual_sum_squares()},
                {"validation_mean_squared_error",
                    fit.has_validation_mean_squared_error()
                        ? Json(fit.validation_mean_squared_error())
                        : Json(nullptr)}});
    }
    return {
        {"plan", decode_strategy_object(value.plan())},
        {"folds", std::move(folds)},
        {"computed_at_ms", value.computed_at_ms()},
        {"scope", "development_validation"},
        {"historical_availability_unknown", true}
    };
}

} // namespace asterion::protocol
