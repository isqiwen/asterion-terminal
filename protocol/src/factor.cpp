#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/factor.hpp>
#include <cmath>
#include <algorithm>
#include <stdexcept>
namespace asterion::protocol {
namespace {
bool digest(const std::string& value) {
  return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
const factor::v1::FactorSeries& series(const factor::v1::FactorInput& input) {
  if (input.series_size() != 1 ||
      input.series(0).source_case() == factor::v1::FactorSeries::SOURCE_NOT_SET)
    throw std::invalid_argument("factor analysis studies exactly one series");
  return input.series(0);
}
unsigned observation_count(const factor::v1::FactorInput& input) {
  const auto& data = series(input);
  return static_cast<unsigned>(data.has_bars() ? data.bars().bars_size()
                                               : data.daily().bars_size());
}
// When an observation was made, as the interface shows it: the bar's label
// timestamp, or the provider's trading date.
std::string observed(const factor::v1::FactorSeries& data, unsigned index) {
  const auto i = static_cast<int>(index);
  return data.has_bars() ? std::to_string(data.bars().bars(i).timestamp_ns())
                         : data.daily().bars(i).trading_day();
}
} // namespace
std::vector<HistoricalDailyBar> daily_factor_bars(const factor::v1::DailyFactorDataset& input) {
  validate_message(input);
  if (input.source_dataset_id() != input.manifest_sha256())
    throw std::invalid_argument("invalid historical dataset revision");
  validate_history_evidence(input.history_evidence());
  if (input.history_evidence().dataset_id() != input.source_dataset_id())
    throw std::invalid_argument("historical evidence does not match dataset versions");
  validate_history_source(input.source());
  (void)HistoryIdentity::parse(input.contract_id());
  if (input.version() != 2 || input.source().empty() || input.contract_id().empty() ||
      input.contract_id().size() > 64 ||
      input.contract_id().find_first_not_of(
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/-") !=
          std::string::npos ||
      !digest(input.manifest_sha256()) || input.bars_size() < 1 || input.bars_size() > 20 * 366 + 1)
    throw std::invalid_argument("invalid daily factor dataset identity");
  std::vector<HistoricalDailyBar> result;
  for (const auto& row : input.bars()) {
    auto bar = daily_bar(row);
    if (row.has_macd() || bar.close <= Decimal{} ||
        (!result.empty() && bar.trading_day <= result.back().trading_day) ||
        bar.volume.raw() % 100000000 || bar.open_interest.raw() % 100000000)
      throw std::invalid_argument("invalid daily factor observations");
    result.push_back(std::move(bar));
  }
  return result;
}
std::string daily_factor_revision(const factor::v1::DailyFactorDataset& input) {
  const auto bars = daily_factor_bars(input);
  Json rows = Json::array();
  const auto optional = [](const std::optional<Decimal>& value) -> Json {
    return value ? Json(value->str()) : Json(nullptr);
  };
  for (const auto& bar : bars)
    rows.push_back({{"date", format_trading_date(bar.trading_day)},
                    {"open", bar.open.str()},
                    {"high", bar.high.str()},
                    {"low", bar.low.str()},
                    {"close", bar.close.str()},
                    {"volume", bar.volume.str()},
                    {"amount", bar.amount.str()},
                    {"open_interest", bar.open_interest.str()},
                    {"previous_close", optional(bar.previous_close)},
                    {"previous_settlement", optional(bar.previous_settlement)},
                    {"settlement", optional(bar.settlement)}});
  return sha256_bytes(Json{
      {"version", 1},
      {"kind", "futures.daily-close"},
      {"source_dataset_id", input.source_dataset_id()},
      {"source", input.source()},
      {"contract_id", input.contract_id()},
      {"manifest_sha256", input.manifest_sha256()},
      {"bars", rows}}.dump());
}
factor::v1::FactorRequest encode_factor_request(const Json& input) {
  require_fields(input, {"series", "lookbacks", "horizon", "evaluation"});
  factor::v1::FactorRequest result;
  if (!input.at("series").is_array() || input.at("series").size() != 1 ||
      !input.at("series").at(0).is_object() || input.at("series").at(0).size() != 1)
    throw std::invalid_argument("factor analysis studies exactly one series");
  const auto& source = input.at("series").at(0);
  if (source.contains("bars"))
    *result.add_series()->mutable_bars() = encode_bar_dataset_request(source.at("bars"));
  else if (source.contains("daily_dataset_id") && source.at("daily_dataset_id").is_string() &&
           digest(source.at("daily_dataset_id").get<std::string>()))
    result.add_series()->set_daily_dataset_id(source.at("daily_dataset_id").get<std::string>());
  else
    throw std::invalid_argument("invalid factor series");
  const auto& windows = input.at("lookbacks");
  if (!windows.is_array() || windows.empty() || windows.size() > 32)
    throw std::invalid_argument("factor requires 1..32 explicit lookback windows");
  unsigned previous = 0;
  for (const auto& window : windows) {
    if (!window.is_number_integer() || window < 1 || window > 10000 || window <= previous)
      throw std::invalid_argument("factor lookbacks must be ascending unique integers in 1..10000");
    previous = window.get<unsigned>();
    result.add_lookbacks(previous);
  }
  if (!input.at("horizon").is_number_integer() || input.at("horizon") < 1 ||
      input.at("horizon") > 10000)
    throw std::invalid_argument("factor horizon must be 1..10000 observations");
  result.set_horizon(input.at("horizon").get<unsigned>());
  const auto& evaluation = input.at("evaluation");
  if (!evaluation.is_object() || !evaluation.contains("mode"))
    throw std::invalid_argument("unsupported factor evaluation mode");
  if (evaluation.at("mode") == "full_sample") {
    require_fields(evaluation, {"mode"});
    result.set_full_sample(true);
  } else if (evaluation.at("mode") == "holdout") {
    require_fields(evaluation, {"mode", "split_index"});
    const auto& split = evaluation.at("split_index");
    if (!split.is_number_integer() || split < 1 || split > 19999)
      throw std::invalid_argument("invalid factor holdout boundary");
    result.set_holdout_start(split.get<unsigned>());
  } else if (evaluation.at("mode") == "walk_forward") {
    require_fields(evaluation, {"mode", "training_events", "validation_events"});
    for (const auto* key : {"training_events", "validation_events"})
      if (!evaluation.at(key).is_number_integer() || evaluation.at(key) < 1 ||
          evaluation.at(key) > 20000)
        throw std::invalid_argument("invalid walk-forward window");
    auto* rolling = result.mutable_walk_forward();
    rolling->set_training_events(evaluation.at("training_events").get<unsigned>());
    rolling->set_validation_events(evaluation.at("validation_events").get<unsigned>());
  } else {
    throw std::invalid_argument("unsupported factor evaluation mode");
  }
  return result;
}
data::v1::DataRequest factor_series_query(const factor::v1::FactorSeriesRequest& source) {
  data::v1::DataRequest query;
  if (source.has_bars())
    *query.mutable_bar_dataset() = source.bars();
  else if (source.has_daily_dataset_id())
    query.mutable_daily_factor_dataset()->set_id(source.daily_dataset_id());
  else
    throw std::invalid_argument("invalid factor series");
  return query;
}
factor::v1::FactorSeries factor_series(const factor::v1::FactorSeriesRequest& source,
                                       data::v1::DataResponse reply) {
  factor::v1::FactorSeries result;
  if (source.has_bars() && reply.has_bar_dataset())
    *result.mutable_bars() = std::move(*reply.mutable_bar_dataset());
  else if (source.has_daily_dataset_id() && reply.has_daily_factor_dataset())
    *result.mutable_daily() = std::move(*reply.mutable_daily_factor_dataset());
  else
    throw std::invalid_argument("data reply does not match the requested factor series");
  return result;
}
std::string factor_series_revision(const factor::v1::FactorSeries& data) {
  if (data.has_bars())
    return data.bars().revision();
  if (data.has_daily())
    return daily_factor_revision(data.daily());
  throw std::invalid_argument("factor analysis studies exactly one series");
}
FactorObservations factor_observations(const factor::v1::FactorInput& input) {
  const auto& data = series(input);
  FactorObservations result;
  if (data.has_bars()) {
    for (const auto& bar : data.bars().bars()) {
      result.closes.push_back(Decimal::from_raw(bar.close().units()));
      result.order.push_back(bar.timestamp_ns());
    }
  } else {
    for (const auto& bar : daily_factor_bars(data.daily())) {
      result.closes.push_back(bar.close);
      result.order.push_back(std::chrono::sys_days(bar.trading_day).time_since_epoch().count());
    }
  }
  return result;
}
namespace {
Json factor_definition(const factor::v1::FactorInput& input) {
  validate_message(input);
  if (input.version() != 6)
    throw std::invalid_argument("unsupported factor input version");
  const auto count = observation_count(input);
  if (!input.horizon() || input.horizon() > 10000 || input.lookbacks_size() < 1 ||
      input.lookbacks_size() > 32)
    throw std::invalid_argument("invalid factor windows");
  unsigned previous = 0;
  Json windows = Json::array();
  for (auto window : input.lookbacks()) {
    if (window <= previous || window > 10000)
      throw std::invalid_argument("factor lookbacks must be ascending unique integers in 1..10000");
    previous = window;
    windows.push_back(window);
  }
  if (input.lookbacks_size() > 1 && !input.has_holdout_start() && !input.has_walk_forward())
    throw std::invalid_argument("factor parameter comparison requires a holdout");
  Json result{{"version", 6},
              {"dataset_revision", input.dataset_revision()},
              {"lookbacks", windows},
              {"horizon", input.horizon()}};
  if (input.has_full_sample() && input.full_sample())
    result["evaluation"] = {{"mode", "full_sample"}};
  else if (input.has_holdout_start() && input.holdout_start() > 0 && input.holdout_start() < count)
    result["evaluation"] = {{"mode", "holdout"}, {"split_index", input.holdout_start()}};
  else if (input.has_walk_forward() && input.walk_forward().training_events() > 0 &&
           input.walk_forward().training_events() <= 20000 &&
           input.walk_forward().validation_events() > 0 &&
           input.walk_forward().validation_events() <= 20000)
    result["evaluation"] = {{"mode", "walk_forward"},
                            {"training_events", input.walk_forward().training_events()},
                            {"validation_events", input.walk_forward().validation_events()}};
  else
    throw std::invalid_argument("explicit factor evaluation required");
  return result;
}
// The structure of a result, without reference to the input it belongs to.
Json result_structure(const factor::v1::FactorResult& result) {
  validate_message(result);
  if (result.version() != 6)
    throw std::invalid_argument("unsupported factor result version");
  for (const auto& row : result.samples())
    if (!std::isfinite(row.value()) || !std::isfinite(row.forward_return()))
      throw std::invalid_argument("non-finite factor result");
  auto statistic = [](bool present, double value) -> Json {
    if (!present)
      return nullptr;
    if (!std::isfinite(value) || value < -1 || value > 1)
      throw std::invalid_argument("invalid correlation result");
    return value;
  };
  Json partitions = Json::array();
  for (const auto& p : result.partitions()) {
    if (p.name() != "full_sample" && p.name() != "development" && p.name() != "holdout")
      throw std::invalid_argument("invalid factor partition");
    if (p.begin_index() >= p.end_index() || p.end_index() > result.input_count() ||
        p.sample_count() > p.end_index() - p.begin_index())
      throw std::invalid_argument("invalid factor partition range");
    partitions.push_back({{"name", p.name()},
                          {"begin_index", p.begin_index()},
                          {"end_index", p.end_index()},
                          {"sample_count", p.sample_count()},
                          {"pearson", statistic(p.has_pearson(), p.pearson())},
                          {"spearman", statistic(p.has_spearman(), p.spearman())}});
  }
  if ((partitions.empty() && result.folds().empty()) || partitions.size() > 2)
    throw std::invalid_argument("missing factor evaluation partitions");
  Json candidates = Json::array();
  for (const auto& c : result.candidates())
    candidates.push_back({{"lookback", c.lookback()},
                          {"sample_count", c.sample_count()},
                          {"development_spearman",
                           statistic(c.has_development_spearman(), c.development_spearman())}});
  if (result.selection_rule() != "fixed" && result.selection_rule() != "development_abs_spearman" &&
      result.selection_rule() != "rolling_fixed" &&
      result.selection_rule() != "rolling_development_abs_spearman")
    throw std::invalid_argument("invalid factor selection rule");
  Json folds = Json::array();
  if (!result.folds().empty()) {
    if (result.folds_size() < 2 || result.folds_size() > 16 || !partitions.empty() ||
        result.lookback() != 0 || !candidates.empty() ||
        (result.selection_rule() != "rolling_fixed" &&
         result.selection_rule() != "rolling_development_abs_spearman"))
      throw std::invalid_argument("invalid walk-forward result structure");
    unsigned previous_end = 0;
    for (const auto& fold : result.folds()) {
      if (fold.training_begin() >= fold.training_end() ||
          fold.training_end() >= fold.validation_end() ||
          fold.validation_end() > result.input_count() ||
          (previous_end && fold.training_end() != previous_end) ||
          fold.development().begin_index() != fold.training_begin() ||
          fold.development().end_index() != fold.training_end() ||
          fold.holdout().begin_index() != fold.training_end() ||
          fold.holdout().end_index() != fold.validation_end() ||
          fold.development().name() != "development" || fold.holdout().name() != "holdout" ||
          !fold.lookback())
        throw std::invalid_argument("invalid walk-forward fold");
      factor::v1::FactorResult metadata;
      metadata.set_version(6);
      metadata.set_input_count(result.input_count());
      metadata.set_selection_rule("fixed");
      *metadata.add_partitions() = fold.development();
      *metadata.add_partitions() = fold.holdout();
      *metadata.mutable_candidates() = fold.candidates();
      const auto decoded = result_structure(metadata);
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
  return {{"version", 6},
          {"folds", folds},
          {"dataset_revision", result.dataset_revision()},
          {"engine_version", result.engine_version()},
          {"lookback", result.lookback()},
          {"horizon", result.horizon()},
          {"input_count", result.input_count()},
          {"partitions", partitions},
          {"purged_count", result.purged_count()},
          {"selection_rule", result.selection_rule()},
          {"evaluation_warmup", result.evaluation_warmup()},
          {"candidates", candidates}};
}
} // namespace
void validate_factor_input(const factor::v1::FactorInput& input) {
  static_cast<void>(factor_definition(input));
  const auto& data = series(input);
  if (data.has_bars()) {
    validate_bar_dataset(data.bars());
    for (const auto& bar : data.bars().bars())
      if (bar.close().units() <= 0)
        throw std::invalid_argument("factor analysis requires positive closes");
  }
  // Recomputing a daily revision validates its observations too.
  if (input.dataset_revision() != factor_series_revision(data))
    throw std::invalid_argument("factor dataset revision mismatch");
  const auto count = observation_count(input);
  const auto warmup = input.lookbacks(input.lookbacks_size() - 1);
  if (count < warmup + input.horizon() + 30)
    throw std::invalid_argument("factor analysis requires at least 30 labelled "
                                "observations after warmup and tail exclusion");
  if (input.has_holdout_start()) {
    const auto split = input.holdout_start();
    if (split < warmup + input.horizon() + 30 || count - split < input.horizon() + 30)
      throw std::invalid_argument(
          "each factor partition requires at least 30 labelled observations");
  }
  if (input.has_walk_forward()) {
    const auto training = input.walk_forward().training_events();
    const auto validation = input.walk_forward().validation_events();
    if (count > 10000 || training > 10000 || validation > 10000 ||
        training < warmup + input.horizon() + 30 || validation < input.horizon() + 30 ||
        count <= training || (count - training) % validation != 0)
      throw std::invalid_argument(
          "walk-forward requires complete windows with at least 30 labelled "
          "observations per partition");
    const auto folds = (count - training) / validation;
    if (folds < 2 || folds > 16)
      throw std::invalid_argument("walk-forward requires 2..16 validation folds");
  }
}
std::size_t factor_work_units(const factor::v1::FactorInput& input) {
  const std::size_t count = observation_count(input);
  const auto candidates = input.lookbacks_size() > 1 ? input.lookbacks_size() : 0;
  if (input.has_walk_forward()) {
    const auto training = input.walk_forward().training_events();
    const auto validation = input.walk_forward().validation_events();
    return (count - training) / validation * (training + validation + candidates * training);
  }
  return count + candidates * input.holdout_start();
}
void validate_factor_result(const factor::v1::FactorInput& input,
                            const factor::v1::FactorResult& result) {
  static_cast<void>(factor_definition(input));
  (void)result_structure(result);
  const auto reject = [] {
    throw std::invalid_argument("factor result evidence does not match its input");
  };
  const auto count = observation_count(input);
  const auto warmup = input.lookbacks(input.lookbacks_size() - 1);
  const auto horizon = input.horizon();
  const bool search = input.lookbacks_size() > 1;
  if (result.engine_version() != factor_engine_version ||
      result.dataset_revision() != input.dataset_revision() || result.input_count() != count ||
      result.horizon() != horizon || result.evaluation_warmup() != warmup)
    reject();
  const auto selection = [&](unsigned lookback, const auto& candidates, unsigned training_count) {
    if (std::ranges::find(input.lookbacks(), lookback) == input.lookbacks().end() ||
        candidates.size() != (search ? input.lookbacks_size() : 0))
      reject();
    for (int i = 0; i < candidates.size(); ++i)
      if (candidates[i].lookback() != input.lookbacks(i) ||
          candidates[i].sample_count() != training_count)
        reject();
  };
  const auto partition = [&](const auto& part, const char* name, unsigned begin, unsigned end,
                             unsigned samples) {
    if (part.name() != name || part.begin_index() != begin || part.end_index() != end ||
        part.sample_count() != samples)
      reject();
  };
  int cursor = 0;
  const auto samples = [&](unsigned begin, unsigned end) {
    for (unsigned i = begin; i + horizon < end; ++i)
      if (cursor == result.samples_size() || result.samples(cursor++).event_index() != i)
        reject();
  };
  if (input.has_walk_forward()) {
    const auto training = input.walk_forward().training_events();
    const auto validation = input.walk_forward().validation_events();
    const auto folds = (count - training) / validation;
    if (result.folds_size() != static_cast<int>(folds) ||
        result.selection_rule() !=
            (search ? "rolling_development_abs_spearman" : "rolling_fixed") ||
        result.purged_count() != folds * horizon)
      reject();
    for (unsigned i = 0; i < folds; ++i) {
      const auto& fold = result.folds(static_cast<int>(i));
      const auto begin = i * validation, split = begin + training, end = split + validation;
      if (fold.training_begin() != begin || fold.training_end() != split ||
          fold.validation_end() != end)
        reject();
      selection(fold.lookback(), fold.candidates(), training - warmup - horizon);
      partition(fold.development(), "development", begin, split, training - warmup - horizon);
      partition(fold.holdout(), "holdout", split, end, validation - horizon);
      samples(split, end);
    }
  } else {
    const auto split = input.has_holdout_start() ? input.holdout_start() : count;
    if (!result.folds().empty() ||
        result.partitions_size() != (input.has_holdout_start() ? 2 : 1) ||
        result.selection_rule() != (search ? "development_abs_spearman" : "fixed") ||
        result.purged_count() != (input.has_holdout_start() ? horizon : 0))
      reject();
    selection(result.lookback(), result.candidates(), split - warmup - horizon);
    partition(result.partitions(0), input.has_holdout_start() ? "development" : "full_sample", 0,
              split, split - warmup - horizon);
    samples(warmup, split);
    if (input.has_holdout_start()) {
      partition(result.partitions(1), "holdout", split, count, count - split - horizon);
      samples(split, count);
    }
  }
  if (cursor != result.samples_size())
    reject();
}
Json decode_factor(const factor::v1::FactorInput& input, DatasetView view) {
  auto result = factor_definition(input);
  const auto& data = series(input);
  Json evidence;
  if (data.has_bars()) {
    evidence = {{"kind", "bars"},
                {"dataset", decode_bar_dataset(data.bars(), view)},
                {"data", decode_bar_dataset_range(data.bars())}};
  } else {
    const auto& daily = data.daily();
    const auto count = daily.bars_size();
    if (count < 1)
      throw std::invalid_argument("invalid daily factor dataset identity");
    evidence = {{"kind", "daily"},
                {"data",
                 {{"source_dataset_id", daily.source_dataset_id()},
                  {"history_evidence", decode_history_evidence(daily.history_evidence())},
                  {"source", daily.source()},
                  {"contract_id", daily.contract_id()},
                  {"manifest_sha256", daily.manifest_sha256()},
                  {"count", count},
                  {"first_day", daily.bars(0).trading_day()},
                  {"last_day", daily.bars(count - 1).trading_day()}}}};
  }
  result["series"] = Json::array({std::move(evidence)});
  return result;
}
Json decode_factor_result(const factor::v1::FactorInput& input,
                          const factor::v1::FactorResult& result) {
  validate_factor_result(input, result);
  auto decoded = result_structure(result);
  const auto& data = series(input);
  Json rows = Json::array();
  for (const auto& row : result.samples())
    rows.push_back({{"event_index", row.event_index()},
                    {"observed", observed(data, row.event_index())},
                    {"label", observed(data, row.event_index() + input.horizon())},
                    {"value", row.value()},
                    {"forward_return", row.forward_return()}});
  decoded["samples"] = std::move(rows);
  return decoded;
}
} // namespace asterion::protocol
