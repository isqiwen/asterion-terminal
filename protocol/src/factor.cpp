#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/factor.hpp>
#include <cmath>
namespace asterion::protocol {
namespace {
Json dataset(const research::v1::FactorInput &input) {
  validate_message(input);
  if (input.version() != 4 || !input.has_contract())
    throw std::invalid_argument(
        "incomplete factor dataset or unsupported version");
  auto result =
      decode_dataset(make_trade_dataset(input.contract(), input.ticks()));
  result.erase("revision");
  return result;
}
} // namespace
std::string factor_dataset_revision(const research::v1::FactorInput &input) {
  return sha256_bytes(dataset(input).dump());
}
research::v1::FactorInput encode_factor(const Json &input) {
  require_fields(input, {"version", "dataset_revision", "contract", "ticks",
                         "lookbacks", "horizon", "evaluation"});
  if (!input.at("version").is_number_integer() || input.at("version") != 4)
    throw std::invalid_argument("unsupported factor input version");
  research::v1::FactorInput result;
  result.set_version(4);
  result.set_dataset_revision(input.at("dataset_revision").get<std::string>());
  const auto &windows = input.at("lookbacks");
  if (!windows.is_array() || windows.empty() || windows.size() > 32)
    throw std::invalid_argument(
        "factor requires 1..32 explicit lookback windows");
  unsigned previous = 0;
  for (const auto &window : windows) {
    if (!window.is_number_integer() || window < 1 || window > 10000 ||
        window <= previous)
      throw std::invalid_argument(
          "factor lookbacks must be ascending unique integers in 1..10000");
    previous = window.get<unsigned>();
    result.add_lookbacks(previous);
  }
  if (!input.at("horizon").is_number_integer() || input.at("horizon") < 1 ||
      input.at("horizon") > 10000)
    throw std::invalid_argument("factor horizon must be 1..10000 events");
  result.set_horizon(input.at("horizon").get<unsigned>());
  const auto &evaluation = input.at("evaluation");
  if (evaluation.at("mode") == "full_sample") {
    require_fields(evaluation, {"mode"});
    result.set_full_sample(true);
  } else if (evaluation.at("mode") == "holdout") {
    require_fields(evaluation, {"mode", "split_index"});
    const auto &split = evaluation.at("split_index");
    if (!split.is_number_integer() || split < 1 || split > 9999)
      throw std::invalid_argument("invalid factor holdout boundary");
    result.set_holdout_start(split.get<unsigned>());
  } else if (evaluation.at("mode") == "walk_forward") {
    require_fields(evaluation,
                   {"mode", "training_events", "validation_events"});
    for (const auto *key : {"training_events", "validation_events"})
      if (!evaluation.at(key).is_number_integer() || evaluation.at(key) < 1 ||
          evaluation.at(key) > 10000)
        throw std::invalid_argument("invalid walk-forward window");
    auto *rolling = result.mutable_walk_forward();
    rolling->set_training_events(
        evaluation.at("training_events").get<unsigned>());
    rolling->set_validation_events(
        evaluation.at("validation_events").get<unsigned>());
  } else {
    throw std::invalid_argument("unsupported factor evaluation mode");
  }
  *result.mutable_contract() = encode_contract(input.at("contract"));
  if (!input.at("ticks").is_array() || input.at("ticks").size() > 10000)
    throw std::invalid_argument("invalid factor tick count");
  for (const auto &row : input.at("ticks"))
    *result.add_ticks() = encode_tick(row);
  return result;
}
Json decode_factor(const research::v1::FactorInput &input) {
  auto result = dataset(input);
  result.erase("type");
  if (input.dataset_revision() != sha256_bytes(dataset(input).dump()))
    throw std::invalid_argument("factor dataset revision mismatch");
  if (!input.horizon() || input.horizon() > 10000 ||
      input.lookbacks_size() < 1 || input.lookbacks_size() > 32)
    throw std::invalid_argument("invalid factor windows");
  unsigned previous = 0;
  Json windows = Json::array();
  for (auto window : input.lookbacks()) {
    if (window <= previous || window > 10000)
      throw std::invalid_argument(
          "factor lookbacks must be ascending unique integers in 1..10000");
    previous = window;
    windows.push_back(window);
  }
  if (input.lookbacks_size() > 1 && !input.has_holdout_start() &&
      !input.has_walk_forward())
    throw std::invalid_argument(
        "factor parameter comparison requires a holdout");
  result["dataset_revision"] = input.dataset_revision();
  result["lookbacks"] = windows;
  result["horizon"] = input.horizon();
  result["version"] = 4;
  if (input.has_full_sample() && input.full_sample())
    result["evaluation"] = {{"mode", "full_sample"}};
  else if (input.has_holdout_start() && input.holdout_start() > 0 &&
           input.holdout_start() < static_cast<unsigned>(input.ticks_size()))
    result["evaluation"] = {{"mode", "holdout"},
                            {"split_index", input.holdout_start()}};
  else if (input.has_walk_forward() &&
           input.walk_forward().training_events() > 0 &&
           input.walk_forward().training_events() <= 10000 &&
           input.walk_forward().validation_events() > 0 &&
           input.walk_forward().validation_events() <= 10000)
    result["evaluation"] = {
        {"mode", "walk_forward"},
        {"training_events", input.walk_forward().training_events()},
        {"validation_events", input.walk_forward().validation_events()}};
  else
    throw std::invalid_argument("explicit factor evaluation required");
  return result;
}
Json decode_factor_result(const research::v1::FactorResult &result) {
  validate_message(result);
  if (result.version() != 4)
    throw std::invalid_argument("unsupported factor result version");
  Json rows = Json::array();
  for (const auto &row : result.samples()) {
    if (!std::isfinite(row.value()) || !std::isfinite(row.forward_return()))
      throw std::invalid_argument("non-finite factor result");
    rows.push_back(
        {{"event_index", row.event_index()},
         {"timestamp_ns", std::to_string(row.timestamp_ns())},
         {"label_timestamp_ns", std::to_string(row.label_timestamp_ns())},
         {"value", row.value()},
         {"forward_return", row.forward_return()}});
  }
  auto statistic = [](bool present, double value) -> Json {
    if (!present)
      return nullptr;
    if (!std::isfinite(value) || value < -1 || value > 1)
      throw std::invalid_argument("invalid correlation result");
    return value;
  };
  Json partitions = Json::array();
  for (const auto &p : result.partitions()) {
    if (p.name() != "full_sample" && p.name() != "development" &&
        p.name() != "holdout")
      throw std::invalid_argument("invalid factor partition");
    if (p.begin_index() >= p.end_index() ||
        p.end_index() > result.input_count() ||
        p.sample_count() > p.end_index() - p.begin_index())
      throw std::invalid_argument("invalid factor partition range");
    partitions.push_back(
        {{"name", p.name()},
         {"begin_index", p.begin_index()},
         {"end_index", p.end_index()},
         {"sample_count", p.sample_count()},
         {"pearson", statistic(p.has_pearson(), p.pearson())},
         {"spearman", statistic(p.has_spearman(), p.spearman())}});
  }
  if ((partitions.empty() && result.folds().empty()) || partitions.size() > 2)
    throw std::invalid_argument("missing factor evaluation partitions");
  Json candidates = Json::array();
  for (const auto &c : result.candidates())
    candidates.push_back(
        {{"lookback", c.lookback()},
         {"sample_count", c.sample_count()},
         {"development_spearman",
          statistic(c.has_development_spearman(), c.development_spearman())}});
  if (result.selection_rule() != "fixed" &&
      result.selection_rule() != "development_abs_spearman" &&
      result.selection_rule() != "rolling_fixed" &&
      result.selection_rule() != "rolling_development_abs_spearman")
    throw std::invalid_argument("invalid factor selection rule");
  Json folds = Json::array();
  if (!result.folds().empty()) {
    if (result.folds_size() < 2 || result.folds_size() > 16 ||
        !partitions.empty() || result.lookback() != 0 || !candidates.empty() ||
        (result.selection_rule() != "rolling_fixed" &&
         result.selection_rule() != "rolling_development_abs_spearman"))
      throw std::invalid_argument("invalid walk-forward result structure");
    unsigned previous_end = 0;
    for (const auto &fold : result.folds()) {
      if (fold.training_begin() >= fold.training_end() ||
          fold.training_end() >= fold.validation_end() ||
          fold.validation_end() > result.input_count() ||
          (previous_end && fold.training_end() != previous_end) ||
          fold.development().begin_index() != fold.training_begin() ||
          fold.development().end_index() != fold.training_end() ||
          fold.holdout().begin_index() != fold.training_end() ||
          fold.holdout().end_index() != fold.validation_end() ||
          fold.development().name() != "development" ||
          fold.holdout().name() != "holdout" || !fold.lookback())
        throw std::invalid_argument("invalid walk-forward fold");
      research::v1::FactorResult metadata;
      metadata.set_version(4);
      metadata.set_input_count(result.input_count());
      metadata.set_selection_rule("fixed");
      *metadata.add_partitions() = fold.development();
      *metadata.add_partitions() = fold.holdout();
      *metadata.mutable_candidates() = fold.candidates();
      const auto decoded = decode_factor_result(metadata);
      folds.push_back({{"training_begin", fold.training_begin()},
                       {"training_end", fold.training_end()},
                       {"validation_end", fold.validation_end()},
                       {"lookback", fold.lookback()},
                       {"candidates", decoded.at("candidates")},
                       {"development", decoded.at("partitions").at(0)},
                       {"holdout", decoded.at("partitions").at(1)}});
      previous_end = fold.validation_end();
    }
    if (previous_end != result.input_count())
      throw std::invalid_argument("incomplete walk-forward coverage");
  } else if (result.selection_rule().starts_with("rolling_"))
    throw std::invalid_argument("missing walk-forward folds");
  return {{"version", 4},
          {"folds", folds},
          {"dataset_revision", result.dataset_revision()},
          {"engine_version", result.engine_version()},
          {"lookback", result.lookback()},
          {"horizon", result.horizon()},
          {"input_count", result.input_count()},
          {"samples", rows},
          {"partitions", partitions},
          {"purged_count", result.purged_count()},
          {"selection_rule", result.selection_rule()},
          {"evaluation_warmup", result.evaluation_warmup()},
          {"candidates", candidates}};
}
} // namespace asterion::protocol
