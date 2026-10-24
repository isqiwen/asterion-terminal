#include <asterion/protocol/research_model.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/foundation/decimal.hpp>
#include <cmath>
#include <array>

namespace asterion::protocol {
namespace r = research::proto;

r::ModelReplaySelection encode_model_replay_selection(const Json& input)
{
    require_fields(
        input,
        {"mapping",
            "fold_index",
            "quantity",
            "sides",
            "processing_delay_bars",
            "time_budget_seconds",
            "availability"});
    r::ModelReplaySelection value;
    *value.mutable_mapping() = encode_strategy_object(input.at("mapping"), "model_mapping");
    const auto integer = [&](const char* key, unsigned lower, unsigned upper) {
        const auto& number = input.at(key);
        if (!number.is_number_integer() || number < lower || number > upper)
            throw std::invalid_argument("model replay requires bounded integer selection policies");
        return number.get<unsigned>();
    };
    value.set_fold_index(integer("fold_index", 0, 31));
    if (!input.at("quantity").is_string())
        throw std::invalid_argument("model replay quantity requires explicit decimal text");
    const auto quantity = Decimal::parse(input.at("quantity").get<std::string>());
    if (quantity <= Decimal{} || quantity.raw() % 100000000 != 0)
        throw std::invalid_argument("model replay quantity requires positive integer lots");
    value.mutable_quantity()->set_units(quantity.raw());
    value.set_sides(encode_position_sides(input.at("sides")));
    value.set_processing_delay_bars(integer("processing_delay_bars", 1, 200000));
    value.set_time_budget_seconds(integer("time_budget_seconds", 1, 3600));
    if (input.at("availability") != "assume_original_bar_close")
        throw std::invalid_argument(
            "model replay requires explicit historical-availability assumption");
    value.set_availability(r::ModelReplayDefinition::ASSUME_ORIGINAL_BAR_CLOSE);
    return value;
}

void validate_model_replay_definition(const r::ModelReplayDefinition& value)
{
    validate_message(value);
    validate_model_mapping(value.mapping());
    validate_model_time_input(value.training());
    const auto& mapping = value.mapping();
    const auto& plan = value.training().plan();
    (void)position_sides(value.sides());
    if (mapping.specification().model().kind() != "model_time_validation_result"
        || mapping.plan().SerializeAsString() != model_time_plan_object(plan).SerializeAsString()
        || mapping.source().SerializeAsString() != plan.recipe().source().SerializeAsString()
        || mapping.specification().valid_until_ms() > plan.specification().valid_until_ms()
        || value.model_attempt() == 0
        || value.fold_index() >= static_cast<unsigned>(plan.specification().folds_size())
        || !value.has_quantity() || value.quantity().units() <= 0
        || value.quantity().units() % 100000000 != 0 || value.processing_delay_bars() < 1
        || value.processing_delay_bars() > 200000 || value.time_budget_seconds() < 1
        || value.time_budget_seconds() > 3600
        || value.availability() != r::ModelReplayDefinition::ASSUME_ORIGINAL_BAR_CLOSE
        || value.ByteSizeLong() > 131072)
        throw std::invalid_argument(
            "model replay requires one fixed fold and explicit diagnostic quantity time and availability policies");
    const auto& fold = plan.specification().folds(static_cast<int>(value.fold_index()));
    if (static_cast<std::uint64_t>(fold.training_end()) + value.processing_delay_bars()
        >= fold.validation_end())
        throw std::invalid_argument(
            "model replay processing delay leaves no available validation decision");
    if (mapping.folds_size() != plan.specification().folds_size())
        throw std::invalid_argument("prediction mapping original fold scope mismatch");
    for (int i = 0; i < mapping.folds_size(); ++i)
        if (mapping.folds(i).SerializeAsString()
            != plan.specification().folds(i).SerializeAsString())
            throw std::invalid_argument("prediction mapping original fold scope mismatch");
    for (int i = 0; i < 4; ++i)
        if (!plan.boundaries(static_cast<int>(value.fold_index()) * 4 + i).has_timestamp_ns())
            throw std::invalid_argument(
                "model replay requires original complete minute bar boundaries");
}

Json decode_model_replay_definition(const r::ModelReplayDefinition& value)
{
    validate_model_replay_definition(value);
    return {
        {"mapping", decode_model_mapping(value.mapping())},
        {"training", decode_model_time_input(value.training())},
        {"model_attempt", value.model_attempt()},
        {"fold_index", value.fold_index()},
        {"quantity", Decimal::from_raw(value.quantity().units()).str()},
        {"sides", position_sides_name(position_sides(value.sides()))},
        {"processing_delay_bars", value.processing_delay_bars()},
        {"time_budget_seconds", value.time_budget_seconds()},
        {"availability", "assume_original_bar_close"},
        {"historical_availability_unknown", true},
        {"scope", "post_hoc_conditional"}
    };
}

namespace {

void replay_shape(const r::ModelReplayResult& value)
{
    validate_message(value);
    if (!value.has_fit() || !value.historical_availability_unknown() || value.decisions_size() < 1
        || value.decisions_size() > 200000 || value.fit().training_members_size() < 2)
        throw std::invalid_argument("incomplete model replay evidence");
    const auto& fit = value.fit();
    if (!std::isfinite(fit.intercept()) || !std::isfinite(fit.coefficient())
        || !std::isfinite(fit.training_residual_sum_squares())
        || fit.training_residual_sum_squares() < 0 || !std::isfinite(fit.regularization())
        || fit.regularization() < 0 || !std::isfinite(fit.training_objective())
        || fit.training_objective() != fit.training_residual_sum_squares() + fit.regularization()
        || fit.actual_started_at_ms() <= 0 || fit.actual_fitted_at_ms() <= 0
        || fit.actual_duration_ns() < 0 || fit.actual_duration_ns() > 3600000000000LL)
        throw std::invalid_argument("invalid actual model replay fit evidence");
    if (fit.has_standardization()) {
        const auto& s = fit.standardization();
        if (s.training_samples() != static_cast<unsigned>(fit.training_members_size())
            || !std::isfinite(s.mean()) || !std::isfinite(s.sample_standard_deviation())
            || s.sample_standard_deviation() <= 0)
            throw std::invalid_argument("model replay scale differs from mature training members");
    }
    std::int64_t previous = 0;
    for (int i = 0; i < value.decisions_size(); ++i) {
        const auto& row = value.decisions(i);
        const bool mapped = row.phase() == r::ModelReplayDecision::MAPPED;
        const bool target = mapped && row.intent() != r::ModelMappedRow::NO_NEW_DIRECTION;
        const auto disposition = target
            ? (i + 1 < value.decisions_size() ? r::ModelReplayDecision::NEXT_BAR
                                              : r::ModelReplayDecision::OUTSIDE_REPLAY)
            : r::ModelReplayDecision::NO_NEW_TARGET;
        if (row.source_index() != static_cast<unsigned>(i) || row.boundary_ns() <= previous
            || row.phase() < r::ModelReplayDecision::BEFORE_TRAINING
            || row.phase() > r::ModelReplayDecision::MAPPED
            || (row.has_feature() && !std::isfinite(row.feature()))
            || row.has_prediction() != mapped
            || (mapped
                && (!row.has_feature() || !std::isfinite(row.prediction())
                    || row.intent() < r::ModelMappedRow::LONG_DIRECTION
                    || row.intent() > r::ModelMappedRow::NO_NEW_DIRECTION))
            || (!mapped
                && row.intent()
                    != (row.phase() == r::ModelReplayDecision::EXPIRED
                            ? r::ModelMappedRow::EXPIRED
                            : r::ModelMappedRow::PREDICTION_UNAVAILABLE))
            || row.has_target() != target || row.disposition() != disposition
            || (target ? row.intent_id().empty() : !row.intent_id().empty()))
            throw std::invalid_argument("invalid model replay decision or target disposition");
        previous = row.boundary_ns();
    }
    unsigned previous_fill = 0;
    for (const auto& fill : value.fills()) {
        if (fill.fill_id().empty() || fill.source_index() == 0
            || fill.source_index() >= static_cast<unsigned>(value.decisions_size())
            || fill.decision_source_index() + 1 != fill.source_index()
            || fill.source_index() < previous_fill)
            throw std::invalid_argument("invalid model replay fill association");
        const auto& decision = value.decisions(static_cast<int>(fill.decision_source_index()));
        if (decision.disposition() != r::ModelReplayDecision::NEXT_BAR
            || fill.intent_id() != decision.intent_id()
            || fill.boundary_ns()
                != value.decisions(static_cast<int>(fill.source_index())).boundary_ns())
            throw std::invalid_argument(
                "model replay fill does not follow its original decision boundary");
        previous_fill = fill.source_index();
    }
}

} // namespace

void validate_model_replay_result(
    const r::ModelReplayDefinition& definition,
    const r::ModelReplayResult& value,
    const data::proto::BarDataset& data)
{
    validate_model_replay_definition(definition);
    replay_shape(value);
    const auto& plan = definition.training().plan();
    const auto& scope = plan.specification().folds(static_cast<int>(definition.fold_index()));
    const auto& recipe = plan.recipe().specification();
    const auto& fit = value.fit();
    const auto ready = scope.training_end() + definition.processing_delay_bars();
    if (data.bars_size() != static_cast<int>(plan.recipe().source().observations())
        || value.decisions_size() != static_cast<int>(scope.validation_end())
        || fit.training_members_size()
            != static_cast<int>(plan.training_counts(static_cast<int>(definition.fold_index())))
        || fit.started_index() != scope.training_end() || fit.ready_index() != ready
        || fit.started_boundary_ns()
            != data.bars(static_cast<int>(scope.training_end())).timestamp_ns()
        || fit.ready_boundary_ns() != data.bars(static_cast<int>(ready)).timestamp_ns()
        || fit.has_standardization() != recipe.standardize()
        || (!recipe.fit_intercept() && fit.intercept() != 0)
        || fit.actual_duration_ns()
            > static_cast<std::int64_t>(definition.time_budget_seconds()) * 1000000000)
        throw std::invalid_argument("model replay fit differs from fixed source clock or policy");
    const auto alpha =
        recipe.has_alpha() ? static_cast<double>(recipe.alpha().units()) / 100000000.0 : 0;
    if (fit.regularization()
        != static_cast<double>(
            static_cast<long double>(alpha) * fit.coefficient() * fit.coefficient()))
        throw std::invalid_argument("model replay penalty differs from fixed training definition");
    unsigned previous = 0;
    for (int i = 0; i < fit.training_members_size(); ++i) {
        const auto member = fit.training_members(i);
        if (member < scope.training_begin() || member >= scope.training_end()
            || member < plan.recipe().source().required_history()
            || static_cast<std::uint64_t>(member) + plan.recipe().source().horizon()
                >= scope.training_end()
            || (i && member <= previous))
            throw std::invalid_argument(
                "model replay fitted an ineligible or future training label");
        previous = member;
    }
    const auto sides = position_sides(definition.sides());
    for (int i = 0; i < value.decisions_size(); ++i) {
        const auto& row = value.decisions(i);
        const auto boundary = data.bars(i).timestamp_ns();
        const auto phase = static_cast<unsigned>(i) < scope.training_end()
            ? r::ModelReplayDecision::BEFORE_TRAINING
            : static_cast<unsigned>(i) < ready ? r::ModelReplayDecision::MODEL_PENDING
            : static_cast<unsigned>(i) < scope.validation_begin()
            ? r::ModelReplayDecision::OUTSIDE_VALIDATION
            : boundary >= definition.mapping().specification().valid_until_ms() * 1000000
            ? r::ModelReplayDecision::EXPIRED
            : !row.has_feature()    ? r::ModelReplayDecision::FEATURE_UNAVAILABLE
            : !row.has_prediction() ? r::ModelReplayDecision::NON_FINITE_PREDICTION
                                    : r::ModelReplayDecision::MAPPED;
        if (row.boundary_ns() != boundary || row.phase() != phase)
            throw std::invalid_argument(
                "model replay decision precedes model availability or leaves its validation window");
        if (row.has_target()) {
            const auto quantity = definition.quantity().units();
            const auto expected = row.intent() == r::ModelMappedRow::LONG_DIRECTION
                    && sides != PositionSides::short_only
                ? quantity
                : row.intent() == r::ModelMappedRow::SHORT_DIRECTION
                    && sides != PositionSides::long_only
                ? -quantity
                : 0;
            if (row.target().units() != expected)
                throw std::invalid_argument(
                    "model replay target differs from fixed quantity and side policy");
        }
    }
}

Json decode_model_replay_result(const r::ModelReplayResult& value)
{
    replay_shape(value);
    const auto& fit = value.fit();
    Json members = Json::array(), decisions = Json::array(), fills = Json::array(), scale = nullptr;
    for (const auto member : fit.training_members())
        members.push_back(member);
    if (fit.has_standardization())
        scale = {
            {"training_samples", fit.standardization().training_samples()},
            {"mean", fit.standardization().mean()},
            {"sample_standard_deviation", fit.standardization().sample_standard_deviation()}
        };
    const std::array phases{
        "",
        "before_training",
        "model_pending",
        "outside_validation",
        "feature_unavailable",
        "non_finite_prediction",
        "expired",
        "mapped"
    };
    const std::array intents{
        "",
        "long_direction",
        "short_direction",
        "no_new_direction",
        "prediction_unavailable",
        "expired"
    };
    const std::array dispositions{"", "no_new_target", "next_bar", "outside_replay"};
    for (const auto& row : value.decisions())
        decisions.push_back(
            {{"source_index", row.source_index()},
                {"boundary_ns", std::to_string(row.boundary_ns())},
                {"phase", phases[row.phase()]},
                {"feature", row.has_feature() ? Json(row.feature()) : Json(nullptr)},
                {"prediction", row.has_prediction() ? Json(row.prediction()) : Json(nullptr)},
                {"intent", intents[row.intent()]},
                {"target",
                    row.has_target() ? Json(Decimal::from_raw(row.target().units()).str())
                                     : Json(nullptr)},
                {"intent_id", row.intent_id()},
                {"disposition", dispositions[row.disposition()]}});
    for (const auto& fill : value.fills())
        fills.push_back(
            {{"fill_id", fill.fill_id()},
                {"source_index", fill.source_index()},
                {"boundary_ns", std::to_string(fill.boundary_ns())},
                {"decision_source_index", fill.decision_source_index()},
                {"intent_id", fill.intent_id()}});
    return {
        {"fit",
            {{"training_members", std::move(members)},
                {"intercept", fit.intercept()},
                {"coefficient", fit.coefficient()},
                {"training_residual_sum_squares", fit.training_residual_sum_squares()},
                {"regularization", fit.regularization()},
                {"training_objective", fit.training_objective()},
                {"standardization", std::move(scale)},
                {"started_index", fit.started_index()},
                {"ready_index", fit.ready_index()},
                {"started_boundary_ns", std::to_string(fit.started_boundary_ns())},
                {"ready_boundary_ns", std::to_string(fit.ready_boundary_ns())},
                {"actual_started_at_ms", fit.actual_started_at_ms()},
                {"actual_fitted_at_ms", fit.actual_fitted_at_ms()},
                {"actual_duration_ns", fit.actual_duration_ns()}}},
        {"decisions", std::move(decisions)},
        {"fills", std::move(fills)},
        {"historical_availability_unknown", true},
        {"scope", "post_hoc_conditional"}
    };
}

} // namespace asterion::protocol
