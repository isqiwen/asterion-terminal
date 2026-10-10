#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/factor.hpp>
#include <cmath>
#include <algorithm>
#include <set>
#include <stdexcept>
namespace asterion::protocol {
namespace {
bool digest(const std::string& value) {
  return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
// The bars one series holds in an input.
std::size_t held_bars(const factor::v1::FactorSeries& data) {
  if (data.has_bars())
    return static_cast<std::size_t>(data.bars().bars_size());
  if (data.has_daily())
    return static_cast<std::size_t>(data.daily().bars_size());
  std::size_t bars = 0;
  for (const auto& month : data.dominant().months())
    bars += static_cast<std::size_t>(month.bars_size());
  return bars;
}
// Minutes of one bar of a series; zero for provider trading dates.
unsigned period(const factor::v1::FactorSeries& data) {
  if (data.has_bars())
    return data.bars().interval_minutes();
  if (data.has_dominant() && data.dominant().months_size())
    return data.dominant().months(0).interval_minutes();
  if (data.has_daily())
    return 0;
  throw std::invalid_argument("factor series must be of one kind and one period");
}
// One series, or a cross-section of contracts observed the same way. Nothing
// reads a dominant series before its schedule is known to fit its months:
// Data resolves a series into the months it reads, in roll order.
void validate_series(const google::protobuf::RepeatedPtrField<factor::v1::FactorSeries>& series) {
  if (series.size() != 1 && (series.size() < 3 || series.size() > 20))
    throw std::invalid_argument("factor analysis studies one series or 3..20 contracts");
  for (const auto& data : series) {
    if (period(data) != period(series.Get(0)) || data.has_daily() != series.Get(0).has_daily())
      throw std::invalid_argument("factor series must be of one kind and one period");
    if (!data.has_dominant())
      continue;
    const auto& dominant = data.dominant();
    std::vector<const v1::Contract*> months;
    for (const auto& month : dominant.months()) {
      if (month.interval_minutes() != dominant.months(0).interval_minutes())
        throw std::invalid_argument("invalid dominant series schedule");
      months.push_back(&month.contract());
    }
    validate_dominant_schedule(dominant.schedule(), months);
    if (dominant.schedule().rolls_size() != dominant.months_size())
      throw std::invalid_argument("invalid dominant series schedule");
    for (int i = 0; i < dominant.months_size(); ++i)
      if (dominant.schedule().rolls(i).contract() != static_cast<unsigned>(i))
        throw std::invalid_argument("invalid dominant series schedule");
  }
}
// A series as a factor reads it: when each observation was made, as a number
// that increases along it (a bar's label timestamp, or the provider's trading
// date), and its close. A dominant series reads each trading day from the
// month its schedule names, at the level of its latest month. The term
// structure reads dominant series alone, on the days that have a term point:
// `terms` then holds that day's carry for every observation.
struct Observed {
  std::vector<std::int64_t> order;
  std::vector<Decimal> closes, terms;
};
Observed observed(const factor::v1::FactorSeries& data, v1::Factor factor) {
  Observed result;
  const auto add = [&](std::int64_t when, Decimal close) {
    result.order.push_back(when);
    result.closes.push_back(close);
  };
  if (factor == v1::TERM_STRUCTURE && !data.has_dominant())
    throw std::invalid_argument("the term structure is read of dominant series");
  if (data.has_bars()) {
    for (const auto& bar : data.bars().bars())
      add(bar.timestamp_ns(), Decimal::from_raw(bar.close().units()));
  } else if (data.has_daily()) {
    for (const auto& bar : daily_factor_bars(data.daily()))
      add(std::chrono::sys_days(bar.trading_day).time_since_epoch().count(), bar.close);
  } else {
    const auto& schedule = data.dominant().schedule();
    const auto& rolls = schedule.rolls();
    // The carry each trading day begins with, where it has a term point.
    std::map<std::string, Decimal> carry;
    if (factor == v1::TERM_STRUCTURE) {
      std::vector<const v1::Contract*> months;
      for (const auto& month : data.dominant().months())
        months.push_back(&month.contract());
      carry = term_carries(schedule, months);
    }
    for (int i = 0; i < rolls.size(); ++i) {
      const auto& month = data.dominant().months(static_cast<int>(rolls[i].contract()));
      const auto scale = Decimal::from_raw(rolls[i].factor().units());
      const auto increment = Decimal::from_raw(month.contract().price_increment().units());
      for (const auto& bar : month.bars()) {
        if (bar.trading_day() < rolls[i].trading_day() ||
            (i + 1 < rolls.size() && bar.trading_day() >= rolls[i + 1].trading_day()))
          continue;
        if (factor == v1::TERM_STRUCTURE) {
          const auto term = carry.find(bar.trading_day());
          if (term == carry.end())
            continue;
          result.terms.push_back(term->second);
        }
        add(bar.timestamp_ns(),
            dominant_price(Decimal::from_raw(bar.close().units()), scale, increment));
      }
    }
  }
  return result;
}
unsigned observation_count(const factor::v1::FactorInput& input) {
  return static_cast<unsigned>(factor_observations(input).order.size());
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
  require_fields(input, {"series", "factor", "lookbacks", "horizon", "evaluation"});
  factor::v1::FactorRequest result;
  const auto& series = input.at("series");
  if (!series.is_array() || (series.size() != 1 && (series.size() < 3 || series.size() > 20)))
    throw std::invalid_argument("factor analysis studies one series or 3..20 contracts");
  if (input.at("factor") == "momentum")
    result.set_factor(v1::PRICE_MOMENTUM);
  else if (input.at("factor") == "term_structure")
    result.set_factor(v1::TERM_STRUCTURE);
  else
    throw std::invalid_argument("unknown factor");
  for (const auto& source : series) {
    if (!source.is_object() || source.size() != 1)
      throw std::invalid_argument("invalid factor series");
    if (source.contains("bars"))
      *result.add_series()->mutable_bars() = encode_bar_dataset_request(source.at("bars"));
    else if (source.contains("daily_dataset_id") && source.at("daily_dataset_id").is_string() &&
             digest(source.at("daily_dataset_id").get<std::string>()))
      result.add_series()->set_daily_dataset_id(source.at("daily_dataset_id").get<std::string>());
    else if (source.contains("dominant")) {
      const auto& months = source.at("dominant");
      if (!months.is_array() || months.size() < 2 || months.size() > 20)
        throw std::invalid_argument("a dominant series requires 2 to 20 month contracts");
      auto* series = result.add_series()->mutable_dominant();
      for (const auto& month : months)
        *series->add_months() = encode_bar_dataset_request(month);
    } else
      throw std::invalid_argument("invalid factor series");
    // Bars and dominant series are read alike; provider trading dates are not.
    if (result.series(result.series_size() - 1).has_daily_dataset_id() !=
        result.series(0).has_daily_dataset_id())
      throw std::invalid_argument("factor series must be of one kind and one period");
  }
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
  else if (source.has_dominant())
    *query.mutable_dominant_series() = source.dominant();
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
  else if (source.has_dominant() && reply.has_dominant_series()) {
    // Data returns the months that are dominant at some point, in roll order.
    auto& resolved = *reply.mutable_dominant_series();
    *result.mutable_dominant()->mutable_months() = std::move(*resolved.mutable_datasets());
    *result.mutable_dominant()->mutable_schedule() = std::move(*resolved.mutable_schedule());
  } else
    throw std::invalid_argument("data reply does not match the requested factor series");
  return result;
}
factor::v1::FactorInput factor_input(const factor::v1::FactorRequest& request) {
  if (request.series_size() != 1 && (request.series_size() < 3 || request.series_size() > 20))
    throw std::invalid_argument("factor analysis studies one series or 3..20 contracts");
  factor::v1::FactorInput input;
  input.set_version(8);
  input.set_factor(request.factor());
  *input.mutable_lookbacks() = request.lookbacks();
  input.set_horizon(request.horizon());
  if (request.has_full_sample())
    input.set_full_sample(request.full_sample());
  else if (request.has_walk_forward())
    *input.mutable_walk_forward() = request.walk_forward();
  else if (request.has_holdout_start())
    input.set_holdout_start(request.holdout_start());
  return input;
}
void add_factor_series(factor::v1::FactorInput& input, factor::v1::FactorSeries series) {
  std::size_t bars = 0;
  *input.add_series() = std::move(series);
  for (const auto& data : input.series())
    bars += held_bars(data);
  if (bars > max_dataset_bars)
    throw std::invalid_argument("dataset exceeds 200000 bars; narrow the date range");
}
std::string
factor_revision(const google::protobuf::RepeatedPtrField<factor::v1::FactorSeries>& series) {
  validate_series(series);
  Json revisions = Json::array();
  for (const auto& data : series) {
    if (data.has_bars())
      revisions.push_back(data.bars().revision());
    else if (data.has_daily())
      revisions.push_back(daily_factor_revision(data.daily()));
    else {
      // The months as they were resolved, and which of them is read when.
      Json identity = Json::array();
      for (const auto& month : data.dominant().months())
        identity.push_back(month.revision());
      identity.push_back(decode_dominant_schedule(data.dominant().schedule()));
      identity.push_back(decode_term_points(data.dominant().schedule()));
      revisions.push_back(sha256_bytes(identity.dump()));
    }
  }
  // One series is identified by its own data; several by all of theirs, in order.
  return revisions.size() == 1 ? revisions.at(0).get<std::string>()
                               : sha256_bytes(revisions.dump());
}
FactorObservations factor_observations(const factor::v1::FactorInput& input) {
  validate_series(input.series());
  std::vector<Observed> all;
  for (const auto& data : input.series())
    all.push_back(observed(data, input.factor()));
  const bool terms = input.factor() == v1::TERM_STRUCTURE;
  FactorObservations result;
  result.closes.resize(all.size());
  result.terms.resize(terms ? all.size() : 0);
  std::vector<std::size_t> cursor(all.size());
  for (std::size_t i = 0; i < all.front().order.size(); ++i) {
    cursor.front() = i;
    const auto when = all.front().order[i];
    bool everywhere = true;
    for (std::size_t s = 1; s < all.size() && everywhere; ++s) {
      auto& at = cursor[s];
      while (at < all[s].order.size() && all[s].order[at] < when)
        ++at;
      everywhere = at < all[s].order.size() && all[s].order[at] == when;
    }
    if (!everywhere)
      continue;
    result.order.push_back(when);
    for (std::size_t s = 0; s < all.size(); ++s) {
      result.closes[s].push_back(all[s].closes[cursor[s]]);
      if (terms)
        result.terms[s].push_back(all[s].terms[cursor[s]]);
    }
  }
  return result;
}
namespace {
Json factor_definition(const factor::v1::FactorInput& input) {
  validate_message(input);
  if (input.version() != 8)
    throw std::invalid_argument("unsupported factor input version");
  if (input.factor() != v1::PRICE_MOMENTUM && input.factor() != v1::TERM_STRUCTURE)
    throw std::invalid_argument("unknown factor");
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
  Json result{{"version", 8},
              {"factor", input.factor() == v1::TERM_STRUCTURE ? "term_structure" : "momentum"},
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
  auto ratio = [](bool present, double value) -> Json {
    if (!present)
      return nullptr;
    if (!std::isfinite(value))
      throw std::invalid_argument("invalid correlation result");
    return value;
  };
  for (const auto& row : result.cross_sections()) {
    (void)statistic(row.has_pearson(), row.pearson());
    (void)statistic(row.has_spearman(), row.spearman());
  }
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
                          {"spearman", statistic(p.has_spearman(), p.spearman())},
                          {"pearson_ratio", ratio(p.has_pearson_ratio(), p.pearson_ratio())},
                          {"spearman_ratio", ratio(p.has_spearman_ratio(), p.spearman_ratio())}});
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
  std::size_t bars = 0;
  std::set<std::string> contracts;
  for (const auto& data : input.series()) {
    if (data.has_bars()) {
      validate_bar_dataset(data.bars());
      contracts.insert(data.bars().contract().venue() + '\n' + data.bars().contract().symbol());
    } else if (data.has_dominant()) {
      for (const auto& month : data.dominant().months())
        validate_bar_dataset(month);
      const auto& contract = data.dominant().months(0).contract();
      contracts.insert(contract.venue() + '\n' + contract.product() + "\ndominant");
    } else {
      contracts.insert(data.daily().contract_id());
    }
    bars += held_bars(data);
    for (const auto close : observed(data, input.factor()).closes)
      if (close <= Decimal{})
        throw std::invalid_argument("factor analysis requires positive closes");
  }
  if (bars > max_dataset_bars)
    throw std::invalid_argument("dataset exceeds 200000 bars; narrow the date range");
  if (contracts.size() != static_cast<std::size_t>(input.series_size()))
    throw std::invalid_argument("factor series must be different contracts");
  // Recomputing a daily revision validates its observations too.
  if (input.dataset_revision() != factor_revision(input.series()))
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
        part.sample_count() != samples ||
        (input.series_size() == 1 && (part.has_pearson_ratio() || part.has_spearman_ratio())))
      reject();
  };
  const bool cross = input.series_size() > 1;
  if (cross ? !result.samples().empty() : !result.cross_sections().empty())
    reject();
  const int rows = cross ? result.cross_sections_size() : result.samples_size();
  int cursor = 0;
  const auto samples = [&](unsigned begin, unsigned end) {
    for (unsigned i = begin; i + horizon < end; ++i)
      if (cursor == rows || (cross ? result.cross_sections(cursor).event_index()
                                   : result.samples(cursor).event_index()) != i)
        reject();
      else
        ++cursor;
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
  if (cursor != rows)
    reject();
}
Json decode_factor(const factor::v1::FactorInput& input, DatasetView view) {
  auto result = factor_definition(input);
  result["series"] = Json::array();
  for (const auto& data : input.series()) {
    if (data.has_bars()) {
      result["series"].push_back({{"kind", "bars"},
                                  {"dataset", decode_bar_dataset(data.bars(), view)},
                                  {"data", decode_bar_dataset_range(data.bars())}});
      continue;
    }
    if (data.has_dominant()) {
      Json months = Json::array();
      for (const auto& month : data.dominant().months())
        months.push_back({{"dataset", decode_bar_dataset(month, view)},
                          {"data", decode_bar_dataset_range(month)}});
      result["series"].push_back({{"kind", "dominant"},
                                  {"months", std::move(months)},
                                  {"rolls", decode_dominant_schedule(data.dominant().schedule())},
                                  {"terms", data.dominant().schedule().terms_size()},
                                  {"count", observed(data, input.factor()).order.size()}});
      continue;
    }
    const auto& daily = data.daily();
    const auto count = daily.bars_size();
    if (count < 1)
      throw std::invalid_argument("invalid daily factor dataset identity");
    result["series"].push_back(
        {{"kind", "daily"},
         {"data",
          {{"source_dataset_id", daily.source_dataset_id()},
           {"history_evidence", decode_history_evidence(daily.history_evidence())},
           {"source", daily.source()},
           {"contract_id", daily.contract_id()},
           {"manifest_sha256", daily.manifest_sha256()},
           {"count", count},
           {"first_day", daily.bars(0).trading_day()},
           {"last_day", daily.bars(count - 1).trading_day()}}}});
  }
  return result;
}
Json decode_factor_result(const factor::v1::FactorInput& input,
                          const factor::v1::FactorResult& result) {
  validate_factor_result(input, result);
  auto decoded = result_structure(result);
  // When an observation was made, as the interface shows it: the bar's label
  // timestamp, or the provider's trading date.
  const auto order = factor_observations(input).order;
  const auto when = [&](unsigned index) {
    const auto at = order[static_cast<std::size_t>(index)];
    return input.series(0).has_daily()
               ? format_trading_date(std::chrono::sys_days{std::chrono::days{at}})
               : std::to_string(at);
  };
  Json samples = Json::array(), sections = Json::array();
  for (const auto& row : result.samples())
    samples.push_back({{"event_index", row.event_index()},
                       {"observed", when(row.event_index())},
                       {"label", when(row.event_index() + input.horizon())},
                       {"value", row.value()},
                       {"forward_return", row.forward_return()}});
  for (const auto& row : result.cross_sections())
    sections.push_back({{"event_index", row.event_index()},
                        {"observed", when(row.event_index())},
                        {"label", when(row.event_index() + input.horizon())},
                        {"pearson", row.has_pearson() ? Json(row.pearson()) : Json(nullptr)},
                        {"spearman", row.has_spearman() ? Json(row.spearman()) : Json(nullptr)}});
  decoded["samples"] = std::move(samples);
  decoded["cross_sections"] = std::move(sections);
  return decoded;
}
} // namespace asterion::protocol
