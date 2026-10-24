#include <asterion/protocol/research_model.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/research_variable.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace asterion::protocol {
namespace {
namespace r = research::proto;

bool hash(const std::string& s)
{
    return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

bool text(const std::string& s, std::size_t limit)
{
    return !s.empty() && s.size() <= limit && s.find(char(0)) == std::string::npos
        && !std::ranges::all_of(s, [](unsigned char c) {
               return std::isspace(c);
           });
}

void owner(const resource::proto::Owner& value)
{
    validate_message(value);
    validate_id(value.node_id());
    validate_id(value.service_id());
    if (value.domain() != "research")
        throw std::invalid_argument("research model owner required");
}

} // namespace

void validate_model_specification(const r::ModelSpecification& value, bool complete)
{
    validate_message(value);
    const auto characters = std::ranges::count_if(value.name(), [](unsigned char c) {
        return (c & 0xc0) != 0x80;
    });
    if (!text(value.name(), 320) || characters > 80 || value.purpose().size() > 4096
        || value.limitations().size() > 4096 || value.purpose().find(char(0)) != std::string::npos
        || value.limitations().find(char(0)) != std::string::npos || value.ByteSizeLong() > 16384
        || value.valid_until_ms() < 0
        || (value.method() != r::MODEL_METHOD_UNSPECIFIED && value.method() != r::MODEL_LINEAR
            && value.method() != r::MODEL_RIDGE)
        || (value.weights() != r::ModelSpecification::WEIGHTS_UNSPECIFIED
            && value.weights() != r::ModelSpecification::EQUAL)
        || (value.mode() != r::ModelSpecification::MODE_UNSPECIFIED
            && value.mode() != r::ModelSpecification::POST_HOC_DIAGNOSTIC)
        || (value.has_alpha() && value.alpha().units() < 0))
        throw std::invalid_argument("invalid model specification fields");
    if (value.has_source())
        validate_strategy_object(value.source(), "factor_result");
    if (!complete)
        return;
    if (!text(value.purpose(), 4096) || !text(value.limitations(), 4096) || !value.has_source()
        || !value.has_fit_intercept() || value.weights() != r::ModelSpecification::EQUAL
        || value.mode() != r::ModelSpecification::POST_HOC_DIAGNOSTIC
        || value.training_begin() >= value.training_end() || value.training_end() > 200000
        || !value.run_budget() || !value.time_budget_seconds() || !value.valid_until_ms()
        || (value.method() == r::MODEL_LINEAR
                ? value.has_alpha()
                : value.method() != r::MODEL_RIDGE || !value.has_alpha()))
        throw std::invalid_argument(
            "model requires explicit supported training conditions and budgets");
}

std::string model_draft_digest(r::ModelDraft value)
{
    value.clear_digest();
    value.clear_updated_at_ms();
    return sha256_bytes(value.SerializeAsString());
}

void validate_model_draft(const r::ModelDraft& value)
{
    validate_message(value);
    validate_id(value.id());
    owner(value.owner());
    validate_model_specification(value.specification(), false);
    if (value.updated_at_ms() < 0 || value.digest() != model_draft_digest(value))
        throw std::invalid_argument("invalid model draft identity");
}

void validate_model_source(const r::ModelSource& value)
{
    validate_message(value);
    validate_strategy_object(value.result(), "factor_result");
    if (!hash(value.input_sha256()) || !hash(value.program_artifact()) || !value.input_bytes()
        || !value.attempt() || !value.observations() || value.observations() > 200000
        || !value.holdout_start() || value.holdout_start() >= value.observations()
        || !value.horizon() || value.horizon() >= value.observations()
        || value.required_history() >= value.observations() || !value.lookback()
        || value.price_power() != 0 || !value.historical_availability_unknown())
        throw std::invalid_argument("invalid fixed model source evidence");
    if (value.has_variable_definition()) {
        validate_strategy_object(value.variable_definition(), "research_variable");
        if (value.factor() != proto::FACTOR_UNSPECIFIED
            || value.variable_definition().owner().SerializeAsString()
                != value.result().owner().SerializeAsString())
            throw std::invalid_argument("model feature definition owner or method mismatch");
    } else if (value.factor() != proto::PRICE_MOMENTUM && value.factor() != proto::VOLATILITY)
        throw std::invalid_argument("model requires a supported dimensionless feature");
}

void validate_model_source(
    const r::ModelSource& source,
    const factor::proto::FactorInput& input,
    const factor::proto::FactorResult& result)
{
    validate_model_source(source);
    validate_factor_result(input, result);
    if (input.series_size() != 1 || input.lookbacks_size() != 1 || !input.has_holdout_start()
        || !input.retain_observations() || result.selection_rule() != "fixed"
        || result.observations_size() != static_cast<int>(result.input_count())
        || source.observations() != result.input_count()
        || source.holdout_start() != input.holdout_start()
        || source.required_history() != result.evaluation_warmup()
        || source.horizon() != input.horizon() || source.lookback() != input.lookbacks(0)
        || source.has_variable_definition() != input.has_variable_definition()
        || source.factor() != input.factor())
        throw std::invalid_argument(
            "model requires original retained fixed-window holdout evidence");
    if (input.has_variable_definition()
        && (input.variable_definition().price_power() != 0
            || source.variable_definition().SerializeAsString()
                != variable_object(input.variable_definition()).SerializeAsString()))
        throw std::invalid_argument(
            "model feature unit or definition does not match original input");
}

std::string model_plan_digest(r::ModelPlan value)
{
    value.clear_digest();
    value.clear_recorded_at_ms();
    return sha256_bytes(value.SerializeAsString());
}

void validate_model_plan(const r::ModelPlan& value)
{
    validate_message(value);
    validate_id(value.id());
    validate_id(value.draft_id());
    owner(value.owner());
    validate_model_specification(value.specification());
    validate_model_source(value.source());
    r::ModelDraft original;
    original.set_id(value.draft_id());
    *original.mutable_owner() = value.owner();
    *original.mutable_specification() = value.specification();
    if (value.draft_digest() != model_draft_digest(original)
        || value.owner().SerializeAsString() != value.source().result().owner().SerializeAsString()
        || value.specification().source().SerializeAsString()
            != value.source().result().SerializeAsString()
        || value.specification().training_end() > value.source().holdout_start()
        || value.training_samples() < 2
        || value.training_samples()
            > value.specification().training_end() - value.specification().training_begin()
        || !hash(value.program_artifact()) || value.recorded_at_ms() < 0
        || value.specification().valid_until_ms() <= value.recorded_at_ms()
        || value.digest() != model_plan_digest(value))
        throw std::invalid_argument(
            "model plan source, time boundary or immutable identity mismatch");
}

resource::proto::Object model_plan_object(const r::ModelPlan& value)
{
    validate_model_plan(value);
    resource::proto::Object result;
    *result.mutable_owner() = value.owner();
    result.set_kind("model_plan");
    result.set_id(value.id());
    result.set_version(value.digest());
    return result;
}

void validate_model_mapping_specification(const r::ModelMappingSpecification& value)
{
    validate_message(value);
    if (!text(value.name(), 256) || !text(value.purpose(), 2048) || !value.has_short_threshold()
        || !value.has_long_threshold()
        || value.short_threshold().units() >= value.long_threshold().units()
        || value.middle() != r::ModelMappingSpecification::NO_NEW_DIRECTION
        || value.valid_until_ms() <= 0 || value.valid_until_ms() > 9223372036854LL)
        throw std::invalid_argument("invalid fixed prediction mapping specification");
    validate_strategy_object(
        value.model(),
        value.model().kind() == "model_time_validation_result" ? "model_time_validation_result"
                                                               : "model_training_result");
    // Computation uses the model's binary64 prediction representation. Distinct
    // Decimal thresholds must remain distinct there; never merge a narrow band.
    if (static_cast<double>(value.short_threshold().units()) / 100000000.0
        >= static_cast<double>(value.long_threshold().units()) / 100000000.0)
        throw std::invalid_argument("prediction threshold band is not representable");
}

std::string model_mapping_digest(r::ModelMapping value)
{
    value.clear_digest();
    value.clear_recorded_at_ms();
    return sha256_bytes(value.SerializeAsString());
}

std::uint32_t model_mapping_rows(const r::ModelMapping& value)
{
    if (value.specification().model().kind() == "model_training_result")
        return value.source().observations();
    std::uint32_t total = 0;
    for (const auto& fold : value.folds())
        total += fold.validation_end() - fold.validation_begin();
    return total;
}

std::pair<std::uint32_t, std::uint32_t>
model_mapping_time_position(const r::ModelMapping& value, std::uint32_t ordinal)
{
    for (int i = 0; i < value.folds_size(); ++i) {
        const auto& fold = value.folds(i);
        const auto count = fold.validation_end() - fold.validation_begin();
        if (ordinal < count)
            return {static_cast<std::uint32_t>(i), fold.validation_begin() + ordinal};
        ordinal -= count;
    }
    throw std::invalid_argument("diagnostic prediction page outside original rows");
}

void validate_model_mapping(const r::ModelMapping& value)
{
    validate_message(value);
    validate_id(value.id());
    owner(value.owner());
    validate_model_mapping_specification(value.specification());
    const bool time = value.specification().model().kind() == "model_time_validation_result";
    validate_strategy_object(value.plan(), time ? "model_time_plan" : "model_plan");
    if (time ? (value.folds_size() < 1 || value.folds_size() > 32) : value.folds_size() != 0)
        throw std::invalid_argument("prediction mapping original fold scope mismatch");
    std::uint32_t previous_end = 0;
    for (const auto& fold : value.folds()) {
        if (fold.training_begin() >= fold.training_end()
            || fold.training_end() > fold.validation_begin()
            || fold.validation_begin() >= fold.validation_end()
            || fold.validation_end() > value.source().holdout_start()
            || fold.validation_begin() < previous_end)
            throw std::invalid_argument("prediction mapping original fold scope mismatch");
        previous_end = fold.validation_end();
    }
    validate_model_source(value.source());
    const auto identity = value.owner().SerializeAsString();
    if (value.specification().model().owner().SerializeAsString() != identity
        || value.plan().owner().SerializeAsString() != identity
        || value.source().result().owner().SerializeAsString() != identity
        || value.model_published_at_ms() <= 0
        || value.model_published_at_ms() > value.recorded_at_ms()
        || value.recorded_at_ms() >= value.specification().valid_until_ms()
        || value.mode() != r::ModelSpecification::POST_HOC_DIAGNOSTIC || !hash(value.digest())
        || value.digest() != model_mapping_digest(value) || value.ByteSizeLong() > 16384)
        throw std::invalid_argument("invalid immutable model prediction mapping");
}

resource::proto::Object model_mapping_object(const r::ModelMapping& value)
{
    validate_model_mapping(value);
    resource::proto::Object result;
    *result.mutable_owner() = value.owner();
    result.set_kind("model_mapping");
    result.set_id(value.id());
    result.set_version(value.digest());
    return result;
}

void validate_model_mutation(const r::ModelMutation& value)
{
    validate_message(value);
    validate_id(value.id());
    if (value.ByteSizeLong() > 32768)
        throw std::invalid_argument("model mutation exceeds metadata capacity");
    if (value.has_save()) {
        validate_id(value.save().draft_id());
        if (!value.save().expected_digest().empty() && !hash(value.save().expected_digest()))
            throw std::invalid_argument("invalid expected model draft identity");
        validate_model_specification(value.save().specification(), false);
    } else if (value.has_fix()) {
        validate_id(value.fix().plan_id());
        validate_id(value.fix().draft_id());
        if (!hash(value.fix().expected_digest()))
            throw std::invalid_argument("fixing model plan requires its exact draft identity");
    } else if (value.has_mapping()) {
        validate_id(value.mapping().mapping_id());
        validate_model_mapping_specification(value.mapping().specification());
    } else if (value.has_time_validation()) {
        validate_id(value.time_validation().plan_id());
        validate_model_time_specification(value.time_validation().specification());
    } else
        throw std::invalid_argument("model metadata mutation required");
}

void validate_model_mutation_reply(
    const r::ModelMutation& request,
    const r::ModelReply& response,
    const resource::proto::Owner& expected)
{
    validate_model_mutation(request);
    validate_message(response);
    owner(expected);
    if (response.drafts_size() || response.plans_size() || response.mappings_size()
        || response.time_plans_size() || !response.next_after().empty()
        || response.ByteSizeLong() > 65536)
        throw std::invalid_argument("invalid model mutation receipt");
    if (request.has_save()) {
        if (!response.has_draft())
            throw std::invalid_argument("model draft receipt missing");
        const auto& draft = response.draft();
        validate_model_draft(draft);
        if (draft.owner().SerializeAsString() != expected.SerializeAsString()
            || draft.id() != request.save().draft_id()
            || draft.specification().SerializeAsString()
                != request.save().specification().SerializeAsString())
            throw std::invalid_argument("model draft receipt does not match original intent");
    } else if (request.has_time_validation()) {
        if (!response.has_time_plan())
            throw std::invalid_argument("fixed model time plan receipt missing");
        const auto& value = response.time_plan();
        validate_model_time_plan(value);
        if (value.owner().SerializeAsString() != expected.SerializeAsString()
            || value.id() != request.time_validation().plan_id()
            || value.specification().SerializeAsString()
                != request.time_validation().specification().SerializeAsString())
            throw std::invalid_argument("model time plan receipt differs from original intent");
    } else if (request.has_mapping()) {
        if (!response.has_mapping())
            throw std::invalid_argument("fixed prediction mapping receipt missing");
        const auto& value = response.mapping();
        validate_model_mapping(value);
        if (value.owner().SerializeAsString() != expected.SerializeAsString()
            || value.id() != request.mapping().mapping_id()
            || value.specification().SerializeAsString()
                != request.mapping().specification().SerializeAsString())
            throw std::invalid_argument(
                "prediction mapping receipt does not match original intent");
    } else {
        if (!response.has_plan())
            throw std::invalid_argument("fixed model plan receipt missing");
        const auto& plan = response.plan();
        validate_model_plan(plan);
        if (plan.owner().SerializeAsString() != expected.SerializeAsString()
            || plan.id() != request.fix().plan_id() || plan.draft_id() != request.fix().draft_id()
            || plan.draft_digest() != request.fix().expected_digest())
            throw std::invalid_argument("fixed model plan receipt does not match original intent");
    }
}

void validate_model_training_input(const r::ModelTrainingInput& value)
{
    validate_message(value);
    validate_model_plan(value.plan());
    if (value.ByteSizeLong() > 65536)
        throw std::invalid_argument("model training input exceeds metadata capacity");
}

std::uint64_t model_training_work_units(const r::ModelTrainingInput& value)
{
    validate_model_training_input(value);
    return std::uint64_t(value.plan().training_samples())
        * (value.plan().specification().standardize() ? 4 : 2)
        + value.plan().source().observations();
}

void validate_model_training_result(
    const r::ModelTrainingInput& input,
    const r::ModelTrainingResult& value)
{
    validate_model_training_input(input);
    validate_message(value);
    const auto& plan = input.plan();
    const auto& spec = plan.specification();
    const auto alpha =
        spec.has_alpha() ? static_cast<double>(spec.alpha().units()) / 100000000.0 : 0;
    const auto penalty = static_cast<double>(
        static_cast<long double>(alpha) * value.coefficient() * value.coefficient());
    if (value.plan().SerializeAsString() != model_plan_object(plan).SerializeAsString()
        || value.training_samples() != plan.training_samples()
        || value.rows_size() != static_cast<int>(plan.source().observations())
        || !std::isfinite(value.intercept()) || !std::isfinite(value.coefficient())
        || !std::isfinite(value.residual_sum_squares()) || !std::isfinite(value.regularization())
        || !std::isfinite(value.objective()) || value.residual_sum_squares() < 0
        || value.regularization() != penalty
        || value.objective() != value.residual_sum_squares() + value.regularization()
        || (!spec.fit_intercept() && value.intercept() != 0)
        || value.fitted_at_ms() < plan.recorded_at_ms()
        || value.fitted_at_ms() >= spec.valid_until_ms())
        throw std::invalid_argument("model result does not match its fixed fit and validity scope");
    if (value.has_standardization() != spec.standardize())
        throw std::invalid_argument("model preprocessing differs from its fixed plan");
    if (value.has_standardization()) {
        const auto& fitted = value.standardization();
        if (fitted.training_samples() != value.training_samples() || !std::isfinite(fitted.mean())
            || !std::isfinite(fitted.sample_standard_deviation())
            || fitted.sample_standard_deviation() <= 0)
            throw std::invalid_argument(
                "model preprocessing requires a complete positive sample scale");
    }
    unsigned fitted = 0;
    for (int i = 0; i < value.rows_size(); ++i) {
        const auto& row = value.rows(i);
        const bool holdout = static_cast<unsigned>(i) >= plan.source().holdout_start();
        const bool training = static_cast<unsigned>(i) >= spec.training_begin()
            && static_cast<unsigned>(i) < spec.training_end();
        const auto role = holdout ? r::ModelTrainingRow::HOLDOUT
            : training            ? r::ModelTrainingRow::TRAINING
                                  : r::ModelTrainingRow::OTHER_DEVELOPMENT;
        if (row.event_index() != static_cast<unsigned>(i) || row.role() != role
            || row.reason() < r::ModelTrainingRow::FITTED
            || row.reason() > r::ModelTrainingRow::CROSSED_LABEL
            || (training ? row.reason() == r::ModelTrainingRow::NOT_TRAINING
                         : row.reason() != r::ModelTrainingRow::NOT_TRAINING)
            || row.prediction_state() < r::ModelTrainingRow::PREDICTED
            || row.prediction_state() > r::ModelTrainingRow::NON_FINITE_PREDICTION
            || row.has_prediction() != (row.prediction_state() == r::ModelTrainingRow::PREDICTED)
            || (row.has_prediction() && !std::isfinite(row.prediction())))
            throw std::invalid_argument(
                "model result row or missing prediction does not match original scope");
        if (row.reason() == r::ModelTrainingRow::FITTED) {
            if (!training || !row.has_prediction())
                throw std::invalid_argument("unqualified model training row");
            ++fitted;
        }
    }
    if (fitted != value.training_samples())
        throw std::invalid_argument("model fit sample count does not match retained rows");
}

r::ModelSpecification encode_model_specification(const Json& input)
{
    require_fields(
        input,
        {"name",
            "purpose",
            "limitations",
            "source",
            "method",
            "fit_intercept",
            "alpha",
            "training_begin",
            "training_end",
            "standardize",
            "weights",
            "run_budget",
            "time_budget_seconds",
            "valid_until_ms",
            "mode"});
    r::ModelSpecification value;
    value.set_name(input.at("name").get<std::string>());
    value.set_purpose(input.at("purpose").get<std::string>());
    value.set_limitations(input.at("limitations").get<std::string>());
    if (!input.at("source").is_null())
        *value.mutable_source() = encode_strategy_object(input.at("source"), "factor_result");
    const auto method = input.at("method").get<std::string>();
    if (method != "linear" && method != "ridge" && method != "unspecified")
        throw std::invalid_argument("invalid model method");
    value.set_method(
        method == "linear"      ? r::MODEL_LINEAR
            : method == "ridge" ? r::MODEL_RIDGE
                                : r::MODEL_METHOD_UNSPECIFIED);
    if (!input.at("fit_intercept").is_null())
        value.set_fit_intercept(input.at("fit_intercept").get<bool>());
    if (!input.at("alpha").is_null())
        value.mutable_alpha()->set_units(
            Decimal::parse(input.at("alpha").get<std::string>()).raw());
    const auto number = [&](const char* key, std::uint64_t max) {
        const auto& n = input.at(key);
        if (!n.is_number_integer() || n < 0 || n > max)
            throw std::invalid_argument("invalid model integer parameter");
        return n.get<std::uint64_t>();
    };
    value.set_training_begin(number("training_begin", 200000));
    value.set_training_end(number("training_end", 200000));
    value.set_standardize(input.at("standardize").get<bool>());
    const auto weights = input.at("weights").get<std::string>();
    if (weights != "equal" && weights != "unspecified")
        throw std::invalid_argument("invalid model weights");
    value.set_weights(
        weights == "equal" ? r::ModelSpecification::EQUAL
                           : r::ModelSpecification::WEIGHTS_UNSPECIFIED);
    value.set_run_budget(number("run_budget", 4294967295ULL));
    value.set_time_budget_seconds(number("time_budget_seconds", 4294967295ULL));
    value.set_valid_until_ms(number("valid_until_ms", 9007199254740991ULL));
    const auto mode = input.at("mode").get<std::string>();
    if (mode != "post_hoc_diagnostic" && mode != "unspecified")
        throw std::invalid_argument("invalid model diagnostic mode");
    value.set_mode(
        mode == "post_hoc_diagnostic" ? r::ModelSpecification::POST_HOC_DIAGNOSTIC
                                      : r::ModelSpecification::MODE_UNSPECIFIED);
    validate_model_specification(value, false);
    return value;
}

Json decode_model_specification(const r::ModelSpecification& value)
{
    validate_model_specification(value, false);
    return {
        {"name", value.name()},
        {"purpose", value.purpose()},
        {"limitations", value.limitations()},
        {"source", value.has_source() ? decode_strategy_object(value.source()) : Json(nullptr)},
        {"method",
            value.method() == r::MODEL_LINEAR      ? "linear"
                : value.method() == r::MODEL_RIDGE ? "ridge"
                                                   : "unspecified"},
        {"fit_intercept", value.has_fit_intercept() ? Json(value.fit_intercept()) : Json(nullptr)},
        {"alpha",
            value.has_alpha() ? Json(Decimal::from_raw(value.alpha().units()).str())
                              : Json(nullptr)},
        {"training_begin", value.training_begin()},
        {"training_end", value.training_end()},
        {"standardize", value.standardize()},
        {"weights", value.weights() == r::ModelSpecification::EQUAL ? "equal" : "unspecified"},
        {"run_budget", value.run_budget()},
        {"time_budget_seconds", value.time_budget_seconds()},
        {"valid_until_ms", value.valid_until_ms()},
        {"mode",
            value.mode() == r::ModelSpecification::POST_HOC_DIAGNOSTIC ? "post_hoc_diagnostic"
                                                                       : "unspecified"}
    };
}

resource::proto::Object model_draft_object(const r::ModelDraft& value)
{
    validate_model_draft(value);
    resource::proto::Object object;
    *object.mutable_owner() = value.owner();
    object.set_kind("model_draft");
    object.set_id(value.id());
    object.set_version(value.digest());
    return object;
}

Json decode_model_draft(const r::ModelDraft& value)
{
    const auto object = model_draft_object(value);
    return {
        {"object", decode_strategy_object(object)},
        {"specification", decode_model_specification(value.specification())},
        {"updated_at_ms", value.updated_at_ms()}
    };
}

Json decode_model_plan(const r::ModelPlan& value)
{
    validate_model_plan(value);
    const auto& source = value.source();
    return {
        {"object", decode_strategy_object(model_plan_object(value))},
        {"draft_id", value.draft_id()},
        {"draft_digest", value.draft_digest()},
        {"specification", decode_model_specification(value.specification())},
        {"program_artifact", value.program_artifact()},
        {"recorded_at_ms", value.recorded_at_ms()},
        {"training_samples", value.training_samples()},
        {"source",
            {{"result", decode_strategy_object(source.result())},
                {"input_sha256", source.input_sha256()},
                {"input_bytes", source.input_bytes()},
                {"attempt", source.attempt()},
                {"program_artifact", source.program_artifact()},
                {"observations", source.observations()},
                {"holdout_start", source.holdout_start()},
                {"horizon", source.horizon()},
                {"required_history", source.required_history()},
                {"lookback", source.lookback()},
                {"price_power", source.price_power()},
                {"factor",
                    source.has_variable_definition() ? "variable" : factor_name(source.factor())},
                {"variable_definition",
                    source.has_variable_definition()
                        ? decode_strategy_object(source.variable_definition())
                        : Json(nullptr)},
                {"historical_availability_unknown", source.historical_availability_unknown()}}}
    };
}

r::ModelMappingSpecification encode_model_mapping_specification(const Json& input)
{
    require_fields(
        input,
        {"name",
            "purpose",
            "model",
            "short_threshold",
            "long_threshold",
            "middle",
            "valid_until_ms"});
    r::ModelMappingSpecification value;
    value.set_name(input.at("name").get<std::string>());
    value.set_purpose(input.at("purpose").get<std::string>());
    const auto kind = input.at("model").at("kind").get<std::string>();
    *value.mutable_model() = encode_strategy_object(
        input.at("model"),
        kind == "model_time_validation_result" ? "model_time_validation_result"
                                               : "model_training_result");
    value.mutable_short_threshold()->set_units(
        Decimal::parse(input.at("short_threshold").get<std::string>()).raw());
    value.mutable_long_threshold()->set_units(
        Decimal::parse(input.at("long_threshold").get<std::string>()).raw());
    if (input.at("middle") != "no_new_direction")
        throw std::invalid_argument("explicit no-new-direction mapping policy required");
    value.set_middle(r::ModelMappingSpecification::NO_NEW_DIRECTION);
    if (!input.at("valid_until_ms").is_number_integer())
        throw std::invalid_argument("prediction mapping validity must be integer UTC milliseconds");
    value.set_valid_until_ms(input.at("valid_until_ms").get<std::int64_t>());
    validate_model_mapping_specification(value);
    return value;
}

Json decode_model_mapping(const r::ModelMapping& value)
{
    validate_model_mapping(value);
    Json folds = Json::array();
    for (const auto& fold : value.folds())
        folds.push_back(
            {{"training_begin", fold.training_begin()},
                {"training_end", fold.training_end()},
                {"validation_begin", fold.validation_begin()},
                {"validation_end", fold.validation_end()}});
    const auto& spec = value.specification();
    const auto& source = value.source();
    return {
        {"object", decode_strategy_object(model_mapping_object(value))},
        {"recorded_at_ms", value.recorded_at_ms()},
        {"mode", "post_hoc_diagnostic"},
        {"model_published_at_ms", value.model_published_at_ms()},
        {"folds", std::move(folds)},
        {"plan", decode_strategy_object(value.plan())},
        {"specification",
            {{"name", spec.name()},
                {"purpose", spec.purpose()},
                {"model", decode_strategy_object(spec.model())},
                {"short_threshold", Decimal::from_raw(spec.short_threshold().units()).str()},
                {"long_threshold", Decimal::from_raw(spec.long_threshold().units()).str()},
                {"middle", "no_new_direction"},
                {"valid_until_ms", spec.valid_until_ms()}}},
        {"source",
            {{"result", decode_strategy_object(source.result())},
                {"input_sha256", source.input_sha256()},
                {"price_power", source.price_power()},
                {"horizon", source.horizon()},
                {"historical_availability_unknown", source.historical_availability_unknown()}}}
    };
}

Json decode_model_reply(
    const r::ModelReply& reply,
    const r::ModelRequest& request,
    const resource::proto::Owner& expected)
{
    validate_message(reply);
    owner(expected);
    Json result{
        {"drafts", Json::array()},
        {"plans", Json::array()},
        {"mappings", Json::array()},
        {"time_plans", Json::array()},
        {"next_after", reply.next_after()}
    };
    const auto checked = [&](const auto& value, const auto& decode) {
        if (value.owner().SerializeAsString() != expected.SerializeAsString())
            throw std::invalid_argument("model response owner mismatch");
        return decode(value);
    };
    if (request.has_preview()) {
        const auto& query = request.preview();
        validate_strategy_object(query.mapping(), "model_mapping");
        if (!reply.has_projection() || reply.drafts_size() || reply.plans_size()
            || reply.mappings_size() || reply.time_plans_size() || !reply.next_after().empty())
            throw std::invalid_argument("diagnostic mapping projection response required");
        const auto& projection = reply.projection();
        const auto& mapping = projection.mapping();
        validate_model_mapping(mapping);
        const auto total = model_mapping_rows(mapping);
        if (query.mapping().owner().SerializeAsString() != expected.SerializeAsString()
            || model_mapping_object(mapping).SerializeAsString()
                != query.mapping().SerializeAsString()
            || query.limit() < 1 || query.limit() > 200 || query.begin() >= total
            || projection.total() != total || projection.begin() != query.begin()
            || projection.observed_at_ms() < mapping.recorded_at_ms())
            throw std::invalid_argument("diagnostic mapping page identity mismatch");
        const auto end = std::min(total, query.begin() + query.limit());
        if (static_cast<unsigned>(projection.rows_size()) != end - query.begin()
            || projection.has_next_begin() != (end < total)
            || (projection.has_next_begin() && projection.next_begin() != end))
            throw std::invalid_argument("diagnostic mapping page continuation mismatch");
        Json rows = Json::array();
        const bool expired =
            projection.observed_at_ms() >= mapping.specification().valid_until_ms();
        const char* roles[] = {"", "training", "other_development", "holdout"};
        const char* reasons[] = {
            "",
            "fitted",
            "not_training",
            "warmup",
            "missing_feature",
            "missing_label",
            "crossed_label"
        };
        const char* states[] = {"", "predicted", "feature_unavailable", "non_finite_prediction"};
        const char* intents[] = {
            "",
            "long_direction",
            "short_direction",
            "no_new_direction",
            "prediction_unavailable",
            "expired"
        };
        for (int i = 0; i < projection.rows_size(); ++i) {
            const auto& value = projection.rows(i);
            if (mapping.folds_size()) {
                if (!value.has_time_observation())
                    throw std::invalid_argument("prediction mapping original fold scope mismatch");
                const auto [fold, index] =
                    model_mapping_time_position(mapping, query.begin() + static_cast<unsigned>(i));
                const auto& original = value.time_observation();
                const auto& row = original.observation();
                if (original.fold_index() != fold || row.event_index() != index
                    || row.has_prediction()
                        != (row.exclusion() != r::ModelTimeRow::FEATURE_UNAVAILABLE
                            && row.exclusion() != r::ModelTimeRow::NON_FINITE_PREDICTION)
                    || row.exclusion() < r::ModelTimeRow::INCLUDED
                    || row.exclusion() > r::ModelTimeRow::NON_FINITE_PREDICTION
                    || (row.has_prediction() && !std::isfinite(row.prediction()))
                    || value.intent() < r::ModelMappedRow::LONG_DIRECTION
                    || value.intent() > r::ModelMappedRow::EXPIRED
                    || expired != (value.intent() == r::ModelMappedRow::EXPIRED)
                    || (!expired
                        && (row.has_prediction()
                                ? value.intent() == r::ModelMappedRow::PREDICTION_UNAVAILABLE
                                : value.intent() != r::ModelMappedRow::PREDICTION_UNAVAILABLE)))
                    throw std::invalid_argument("invalid original prediction mapping row");
                const char* exclusions[] = {
                    "",
                    "included",
                    "warmup",
                    "feature_unavailable",
                    "label_unavailable",
                    "crossed_label",
                    "non_finite_prediction"
                };
                rows.push_back(
                    {{"index", index},
                        {"fold_index", fold},
                        {"role", "development_validation"},
                        {"exclusion", exclusions[row.exclusion()]},
                        {"prediction",
                            row.has_prediction() ? Json(row.prediction()) : Json(nullptr)},
                        {"intent", intents[value.intent()]}});
                continue;
            }
            if (!value.has_observation())
                throw std::invalid_argument("invalid original prediction mapping row");
            const auto& row = value.observation();
            if (row.event_index() != query.begin() + static_cast<unsigned>(i)
                || row.role() < r::ModelTrainingRow::TRAINING
                || row.role() > r::ModelTrainingRow::HOLDOUT
                || row.reason() < r::ModelTrainingRow::FITTED
                || row.reason() > r::ModelTrainingRow::CROSSED_LABEL
                || row.prediction_state() < r::ModelTrainingRow::PREDICTED
                || row.prediction_state() > r::ModelTrainingRow::NON_FINITE_PREDICTION
                || row.has_prediction()
                    != (row.prediction_state() == r::ModelTrainingRow::PREDICTED)
                || (row.has_prediction() && !std::isfinite(row.prediction()))
                || value.intent() < r::ModelMappedRow::LONG_DIRECTION
                || value.intent() > r::ModelMappedRow::EXPIRED
                || expired != (value.intent() == r::ModelMappedRow::EXPIRED)
                || (!expired
                    && (row.has_prediction()
                            ? value.intent() == r::ModelMappedRow::PREDICTION_UNAVAILABLE
                            : value.intent() != r::ModelMappedRow::PREDICTION_UNAVAILABLE)))
                throw std::invalid_argument("invalid original prediction mapping row");
            rows.push_back(
                {{"index", row.event_index()},
                    {"role", roles[row.role()]},
                    {"reason", reasons[row.reason()]},
                    {"prediction_state", states[row.prediction_state()]},
                    {"prediction", row.has_prediction() ? Json(row.prediction()) : Json(nullptr)},
                    {"intent", intents[value.intent()]}});
        }
        result["projection"] = {
            {"mapping", decode_model_mapping(mapping)},
            {"rows", std::move(rows)},
            {"total", projection.total()},
            {"begin", projection.begin()},
            {"next_begin",
                projection.has_next_begin() ? Json(projection.next_begin()) : Json(nullptr)},
            {"observed_at_ms", projection.observed_at_ms()},
            {"scope", "post_hoc_diagnostic"}
        };
        return result;
    }
    if (request.has_mutation() || request.has_read()) {
        if (reply.drafts_size() || reply.plans_size() || reply.mappings_size()
            || !reply.next_after().empty())
            throw std::invalid_argument("unexpected model records in single response");
        if (request.has_mutation())
            validate_model_mutation_reply(request.mutation(), reply, expected);
        else {
            const auto& read = request.read();
            const bool matched = (read.kind() == r::ModelRead::DRAFT && reply.has_draft()
                                     && reply.draft().id() == read.id())
                || (read.kind() == r::ModelRead::PLAN && reply.has_plan()
                    && reply.plan().id() == read.id())
                || (read.kind() == r::ModelRead::MAPPING && reply.has_mapping()
                    && reply.mapping().id() == read.id())
                || (read.kind() == r::ModelRead::TIME_PLAN && reply.has_time_plan()
                    && reply.time_plan().id() == read.id());
            if (!matched)
                throw std::invalid_argument("model read response identity mismatch");
        }
        if (reply.has_draft())
            result["draft"] = checked(reply.draft(), decode_model_draft);
        else if (reply.has_plan())
            result["plan"] = checked(reply.plan(), decode_model_plan);
        else if (reply.has_mapping())
            result["mapping"] = checked(reply.mapping(), decode_model_mapping);
        else if (reply.has_time_plan())
            result["time_plan"] = checked(reply.time_plan(), decode_model_time_plan);
        else
            throw std::invalid_argument("model response record missing");
    } else if (request.has_list()) {
        const auto& page = request.list();
        if (page.limit() < 1 || page.limit() > 200
            || reply.record_case() != r::ModelReply::RECORD_NOT_SET
            || (page.kind() != r::ModelRead::DRAFT && page.kind() != r::ModelRead::PLAN
                && page.kind() != r::ModelRead::MAPPING && page.kind() != r::ModelRead::TIME_PLAN)
            || (page.kind() != r::ModelRead::DRAFT && reply.drafts_size())
            || (page.kind() != r::ModelRead::PLAN && reply.plans_size())
            || (page.kind() != r::ModelRead::MAPPING && reply.mappings_size())
            || (page.kind() != r::ModelRead::TIME_PLAN && reply.time_plans_size()))
            throw std::invalid_argument("invalid model page response");
        std::string cursor = page.after();
        Json rows = Json::array();
        const auto append = [&](const auto& values, const auto& decode) {
            for (const auto& value : values) {
                if (value.id() <= cursor)
                    throw std::invalid_argument("invalid model page order");
                cursor = value.id();
                rows.push_back(checked(value, decode));
            }
        };
        if (page.kind() == r::ModelRead::DRAFT)
            append(reply.drafts(), decode_model_draft);
        else if (page.kind() == r::ModelRead::PLAN)
            append(reply.plans(), decode_model_plan);
        else if (page.kind() == r::ModelRead::MAPPING)
            append(reply.mappings(), decode_model_mapping);
        else
            append(reply.time_plans(), decode_model_time_plan);
        if (rows.size() > page.limit()
            || (!reply.next_after().empty()
                && (rows.size() != page.limit() || reply.next_after() != cursor)))
            throw std::invalid_argument("invalid model page continuation");
        result
            [page.kind() == r::ModelRead::DRAFT            ? "drafts"
                    : page.kind() == r::ModelRead::PLAN    ? "plans"
                    : page.kind() == r::ModelRead::MAPPING ? "mappings"
                                                           : "time_plans"] = std::move(rows);
    } else
        throw std::invalid_argument("invalid model response operation");
    return result;
}

Json decode_model_training_input(const r::ModelTrainingInput& value)
{
    validate_model_training_input(value);
    return {{"plan", decode_model_plan(value.plan())}};
}

Json decode_model_training_result(
    const r::ModelTrainingInput& input,
    const r::ModelTrainingResult& value)
{
    validate_model_training_result(input, value);
    Json rows = Json::array();
    for (const auto& row : value.rows()) {
        const char* reasons[] = {
            "",
            "fitted",
            "not_training",
            "warmup",
            "missing_feature",
            "missing_label",
            "crossed_label"
        };
        rows.push_back(
            {{"event_index", row.event_index()},
                {"role",
                    row.role() == r::ModelTrainingRow::TRAINING      ? "training"
                        : row.role() == r::ModelTrainingRow::HOLDOUT ? "holdout"
                                                                     : "other_development"},
                {"reason", reasons[row.reason()]},
                {"prediction_state",
                    row.prediction_state() == r::ModelTrainingRow::PREDICTED ? "predicted"
                        : row.prediction_state() == r::ModelTrainingRow::FEATURE_UNAVAILABLE
                        ? "feature_unavailable"
                        : "non_finite_prediction"},
                {"prediction", row.has_prediction() ? Json(row.prediction()) : Json(nullptr)}});
    }
    Json standardized = nullptr;
    if (value.has_standardization())
        standardized = {
            {"training_samples", value.standardization().training_samples()},
            {"mean", value.standardization().mean()},
            {"sample_standard_deviation", value.standardization().sample_standard_deviation()}
        };
    return {
        {"plan", decode_strategy_object(value.plan())},
        {"training_samples", value.training_samples()},
        {"intercept", value.intercept()},
        {"coefficient", value.coefficient()},
        {"residual_sum_squares", value.residual_sum_squares()},
        {"regularization", value.regularization()},
        {"objective", value.objective()},
        {"fitted_at_ms", value.fitted_at_ms()},
        {"standardization", std::move(standardized)},
        {"rows", std::move(rows)}
    };
}

} // namespace asterion::protocol
