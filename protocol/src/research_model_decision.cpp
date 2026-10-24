#include <asterion/protocol/research_model.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/trading.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>

namespace asterion::protocol {
namespace {
namespace r = research::proto;
constexpr std::array intents{
    "",
    "long_direction",
    "short_direction",
    "no_new_direction",
    "prediction_unavailable",
    "expired"
};
constexpr std::array reasons{
    "",
    "ready",
    "model_not_ready",
    "mapping_not_ready",
    "input_not_ready",
    "feature_unavailable",
    "non_finite_prediction",
    "expired"
};

bool text(const std::string& value, std::size_t limit)
{
    return !value.empty() && value.size() <= limit && value.find(char(0)) == std::string::npos
        && !std::ranges::all_of(value, [](unsigned char c) {
               return std::isspace(c);
           });
}

bool outcome(r::ModelMappedRow::Intent intent, r::ModelDecisionReason reason)
{
    if (reason == r::MODEL_DECISION_READY)
        return intent >= r::ModelMappedRow::LONG_DIRECTION
            && intent <= r::ModelMappedRow::NO_NEW_DIRECTION;
    if (reason == r::MODEL_DECISION_EXPIRED)
        return intent == r::ModelMappedRow::EXPIRED;
    return reason >= r::MODEL_NOT_READY && reason <= r::MODEL_NON_FINITE_PREDICTION
        && intent == r::ModelMappedRow::PREDICTION_UNAVAILABLE;
}

std::int64_t nanoseconds(const Json& value)
{
    const auto raw = value.get<std::string>();
    std::int64_t result;
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size())
        throw std::invalid_argument("model decision times require exact nanosecond strings");
    return result;
}

unsigned integer(const Json& value, unsigned limit)
{
    if (!value.is_number_integer() || value < 0 || value > limit)
        throw std::invalid_argument("model decision parameters require bounded integers");
    return value.get<unsigned>();
}

template <std::size_t N>
unsigned named(const Json& value, const std::array<const char*, N>& names)
{
    const auto name = value.get<std::string>();
    const auto found = std::find(names.begin() + 1, names.end(), name);
    if (found == names.end())
        throw std::invalid_argument("unknown model decision expectation");
    return static_cast<unsigned>(found - names.begin());
}

} // namespace

void validate_model_decision_request(const r::ModelDecisionTrialRequest& value)
{
    validate_message(value);
    validate_strategy_object(value.mapping(), "model_mapping");
    if (!value.fictional() || !text(value.purpose(), 2048) || value.cases_size() < 1
        || value.cases_size() > 1000 || value.time_budget_seconds() < 1
        || value.time_budget_seconds() > 3600
        || (value.has_fold_index() && value.fold_index() >= 32) || value.ByteSizeLong() > 131072)
        throw std::invalid_argument(
            "model decision trial requires explicit bounded fictional cases");
    std::int64_t previous = 0;
    for (const auto& row : value.cases()) {
        if (!text(row.name(), 128) || row.boundary_ns() <= previous
            || row.feature_available_at_ns() < 0
            || (row.has_feature()
                && (!std::isfinite(row.feature()) || row.feature_available_at_ns() == 0))
            || !outcome(row.expected_intent(), row.expected_reason()))
            throw std::invalid_argument("invalid original model decision case or expectation");
        previous = row.boundary_ns();
    }
}

void validate_model_decision_input(const r::ModelDecisionTrialInput& value)
{
    validate_message(value);
    validate_model_decision_request(value.request());
    validate_model_mapping(value.mapping());
    const auto& mapping = value.mapping();
    const auto& request = value.request();
    const bool time = mapping.specification().model().kind() == "model_time_validation_result";
    if (request.mapping().SerializeAsString() != model_mapping_object(mapping).SerializeAsString()
        || value.model_attempt() == 0 || value.ByteSizeLong() > 262144
        || (time ? !value.has_time() : !value.has_training()) || request.has_fold_index() != time)
        throw std::invalid_argument("model decision input differs from its fixed source");
    if (time)
        validate_model_time_input(value.time());
    else
        validate_model_training_input(value.training());
    const auto& recipe = time ? value.time().plan().recipe() : value.training().plan();
    const auto plan =
        time ? model_time_plan_object(value.time().plan()) : model_plan_object(recipe);
    if (plan.SerializeAsString() != mapping.plan().SerializeAsString()
        || recipe.source().SerializeAsString() != mapping.source().SerializeAsString()
        || mapping.specification().valid_until_ms()
            > (time ? value.time().plan().specification().valid_until_ms()
                    : recipe.specification().valid_until_ms())
        || (time
            && request.fold_index()
                >= static_cast<unsigned>(value.time().plan().specification().folds_size())))
        throw std::invalid_argument("model decision source plan or selected fold changed");
    if (time) {
        const auto& folds = value.time().plan().specification().folds();
        if (mapping.folds_size() != folds.size())
            throw std::invalid_argument("prediction mapping original fold scope mismatch");
        for (int i = 0; i < folds.size(); ++i)
            if (mapping.folds(i).SerializeAsString() != folds[i].SerializeAsString())
                throw std::invalid_argument("prediction mapping original fold scope mismatch");
    }
}

void validate_model_decision_result(
    const r::ModelDecisionTrialInput& input,
    const r::ModelDecisionTrialResult& value)
{
    validate_model_decision_input(input);
    validate_message(value);
    if (value.rows_size() != input.request().cases_size()
        || value.computed_at_ms() < input.mapping().recorded_at_ms()
        || value.computed_at_ms() >= input.mapping().specification().valid_until_ms())
        throw std::invalid_argument(
            "model decision result differs from its original cases or validity");
    unsigned matched = 0;
    for (int i = 0; i < value.rows_size(); ++i) {
        const auto& row = value.rows(i);
        const auto& expected = input.request().cases(i);
        const bool matches = row.intent() == expected.expected_intent()
            && row.reason() == expected.expected_reason();
        if (row.index() != static_cast<unsigned>(i) || !outcome(row.intent(), row.reason())
            || row.has_prediction() != (row.reason() == r::MODEL_DECISION_READY)
            || (row.has_prediction() && !std::isfinite(row.prediction()))
            || row.matches() != matches)
            throw std::invalid_argument("invalid original model decision result row");
        matched += matches;
    }
    if (value.matched() != matched
        || value.all_match() != (matched == static_cast<unsigned>(value.rows_size())))
        throw std::invalid_argument("model decision result summary mismatch");
}

r::ModelDecisionTrialRequest encode_model_decision_request(const Json& input)
{
    require_fields(
        input,
        {"mapping", "fold_index", "fictional", "purpose", "cases", "time_budget_seconds"});
    r::ModelDecisionTrialRequest value;
    *value.mutable_mapping() = encode_strategy_object(input.at("mapping"), "model_mapping");
    if (!input.at("fold_index").is_null())
        value.set_fold_index(integer(input.at("fold_index"), 31));
    value.set_fictional(input.at("fictional").get<bool>());
    value.set_purpose(input.at("purpose").get<std::string>());
    value.set_time_budget_seconds(integer(input.at("time_budget_seconds"), 3600));
    const auto& cases = input.at("cases");
    if (!cases.is_array() || cases.empty() || cases.size() > 1000)
        throw std::invalid_argument(
            "model decision trial requires explicit bounded fictional cases");
    for (const auto& item : cases) {
        require_fields(
            item,
            {"name",
                "boundary_ns",
                "feature_available_at_ns",
                "feature",
                "expected_intent",
                "expected_reason"});
        auto& row = *value.add_cases();
        row.set_name(item.at("name").get<std::string>());
        row.set_boundary_ns(nanoseconds(item.at("boundary_ns")));
        row.set_feature_available_at_ns(nanoseconds(item.at("feature_available_at_ns")));
        if (!item.at("feature").is_null()) {
            if (!item.at("feature").is_number())
                throw std::invalid_argument(
                    "model decision feature must be numeric or explicitly missing");
            row.set_feature(item.at("feature").get<double>());
        }
        row.set_expected_intent(
            static_cast<r::ModelMappedRow::Intent>(named(item.at("expected_intent"), intents)));
        row.set_expected_reason(
            static_cast<r::ModelDecisionReason>(named(item.at("expected_reason"), reasons)));
    }
    validate_model_decision_request(value);
    return value;
}

Json decode_model_decision_request(const r::ModelDecisionTrialRequest& value)
{
    validate_model_decision_request(value);
    Json cases = Json::array();
    for (const auto& row : value.cases())
        cases.push_back(
            {{"name", row.name()},
                {"boundary_ns", std::to_string(row.boundary_ns())},
                {"feature_available_at_ns", std::to_string(row.feature_available_at_ns())},
                {"feature", row.has_feature() ? Json(row.feature()) : Json(nullptr)},
                {"expected_intent", intents[row.expected_intent()]},
                {"expected_reason", reasons[row.expected_reason()]}});
    return {
        {"mapping", decode_strategy_object(value.mapping())},
        {"fold_index", value.has_fold_index() ? Json(value.fold_index()) : Json(nullptr)},
        {"fictional", true},
        {"purpose", value.purpose()},
        {"cases", std::move(cases)},
        {"time_budget_seconds", value.time_budget_seconds()}
    };
}

Json decode_model_decision_input(const r::ModelDecisionTrialInput& value)
{
    validate_model_decision_input(value);
    return {
        {"mapping", decode_model_mapping(value.mapping())},
        {"request", decode_model_decision_request(value.request())},
        {"model_attempt", value.model_attempt()},
        {"source",
            value.has_time() ? decode_model_time_input(value.time())
                             : decode_model_training_input(value.training())}
    };
}

Json decode_model_decision_result(
    const r::ModelDecisionTrialInput& input,
    const r::ModelDecisionTrialResult& value)
{
    validate_model_decision_result(input, value);
    Json rows = Json::array();
    for (const auto& row : value.rows())
        rows.push_back(
            {{"index", row.index()},
                {"prediction", row.has_prediction() ? Json(row.prediction()) : Json(nullptr)},
                {"intent", intents[row.intent()]},
                {"reason", reasons[row.reason()]},
                {"matches", row.matches()}});
    return {
        {"rows", std::move(rows)},
        {"matched", value.matched()},
        {"all_match", value.all_match()},
        {"computed_at_ms", value.computed_at_ms()},
        {"scope", "fictional_fixed_model_decision"}
    };
}

} // namespace asterion::protocol
