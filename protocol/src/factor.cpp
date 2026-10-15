#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/factor.hpp>
#include <cmath>
#include <charconv>
#include <algorithm>
#include <set>
#include <stdexcept>
namespace asterion::protocol {
namespace {
void validate_liquidity(const factor::v1::LiquidityRule& rule) {
  if (!rule.minimum_history() || rule.minimum_history() > 10000 || rule.minimum_volume() < 0 ||
      rule.minimum_open_interest() < 0)
    throw std::invalid_argument("invalid liquidity eligibility rule");
}
Json liquidity_definition(const factor::v1::LiquidityRule& rule) {
  validate_liquidity(rule);
  return {{"minimum_history", rule.minimum_history()},
          {"minimum_volume", std::to_string(rule.minimum_volume())},
          {"minimum_open_interest", std::to_string(rule.minimum_open_interest())}};
}
bool digest(const std::string& value) {
  return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
// The bars one series holds in an input.
std::size_t held_bars(const factor::v1::FactorSeries& data) {
  if (data.has_bars())
    return static_cast<std::size_t>(data.bars().bars_size());
  std::size_t bars = 0;
  for (const auto& month : data.table().months())
    bars += static_cast<std::size_t>(month.closes_size());
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
  if (data.has_table())
    return 0;
  throw std::invalid_argument("factor series must be of one kind and one period");
}
// Series are compared only with series observed the same way: bars with bars
// (a dominant series is bars), and the market-wide table with itself.
factor::v1::FactorSeries::SourceCase observed_as(const factor::v1::FactorSeries& data) {
  return data.has_dominant() ? factor::v1::FactorSeries::kBars : data.source_case();
}
// Whether a series of the market-wide table is a product's dominant series.
bool table_product(const factor::v1::FactorSeries& data) {
  return data.has_table() && data.table().has_schedule();
}
// One series, or a cross-section of contracts observed the same way. Nothing
// reads a dominant series before its schedule is known to fit its months:
// Data resolves a series into the months it reads, in roll order.
void validate_series(const google::protobuf::RepeatedPtrField<factor::v1::FactorSeries>& series) {
  if (series.size() != 1 && (series.size() < 3 || series.size() > max_factor_series))
    throw std::invalid_argument("factor analysis studies one series or 3..100 contracts");
  for (const auto& data : series) {
    if (period(data) != period(series.Get(0)) || observed_as(data) != observed_as(series.Get(0)))
      throw std::invalid_argument("factor series must be of one kind and one period");
    if (data.has_table())
      validate_table_series(data.table());
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
  if (factor == v1::TERM_STRUCTURE && !data.has_dominant() && !table_product(data))
    throw std::invalid_argument("the term structure is read of dominant series");
  if (data.has_table()) {
    // Closes by provider trading date. A product's months are read while they
    // are dominant and multiplied to the level of the last one; the table has
    // no price increment to bring the result back onto.
    const auto& table = data.table();
    const auto months = table_series_months(table);
    std::map<std::string, Decimal> carry;
    if (factor == v1::TERM_STRUCTURE) {
      std::vector<const v1::Contract*> contracts;
      for (const auto& month : months)
        contracts.push_back(&month);
      carry = term_carries(table.schedule(), contracts);
    }
    for (int m = 0; m < table.months_size(); ++m) {
      const auto scale = table.has_schedule()
                             ? Decimal::from_raw(table.schedule().rolls(m).factor().units())
                             : Decimal::parse("1");
      for (const auto& close : table.months(m).closes()) {
        if (factor == v1::TERM_STRUCTURE) {
          const auto term = carry.find(close.trading_day());
          if (term == carry.end())
            continue;
          result.terms.push_back(term->second);
        }
        add(std::chrono::sys_days(parse_trading_date(close.trading_day()))
                .time_since_epoch()
                .count(),
            multiply(Decimal::from_raw(close.close().units()), scale, Rounding::half_up));
      }
    }
  } else if (data.has_bars()) {
    for (const auto& bar : data.bars().bars())
      add(bar.timestamp_ns(), Decimal::from_raw(bar.close().units()));
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
      auto increment = Decimal::from_raw(month.contract().price_increment().units());
      auto change = month.price_increment_changes().begin();
      for (const auto& bar : month.bars()) {
        // Back on the price increment of the bar's day.
        for (; change != month.price_increment_changes().end() &&
               change->trading_day() <= bar.trading_day();
             ++change)
          increment = Decimal::from_raw(change->value().units());
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
factor::v1::FactorRequest encode_factor_request(const Json& input) {
  auto required = input;
  required.erase("liquidity");
  require_fields(required, {"series", "factor", "lookbacks", "horizon", "evaluation"});
  factor::v1::FactorRequest result;
  const auto& series = input.at("series");
  if (!series.is_array() || series.empty() ||
      series.size() > static_cast<std::size_t>(max_factor_series))
    throw std::invalid_argument("factor analysis studies one series or 3..100 contracts");
  result.set_factor(encode_factor(input.at("factor")));
  for (const auto& source : series) {
    if (!source.is_object() || source.size() != 1)
      throw std::invalid_argument("invalid factor series");
    if (source.contains("tables"))
      *result.add_series()->mutable_tables() = encode_table_series_request(source.at("tables"));
    else if (source.contains("bars"))
      *result.add_series()->mutable_bars() = encode_bar_dataset_request(source.at("bars"));
    else if (source.contains("dominant") && source.at("dominant").is_object()) {
      // A product of the daily table: Data finds its months.
      require_fields(source.at("dominant"), {"product"});
      *result.add_series()->mutable_dominant()->mutable_product() =
          encode_product_bars_request(source.at("dominant").at("product"));
    } else if (source.contains("dominant")) {
      const auto& months = source.at("dominant");
      if (!months.is_array() || months.size() < 2 || months.size() > 20)
        throw std::invalid_argument("a dominant series requires 2 to 20 month contracts");
      auto* series = result.add_series()->mutable_dominant();
      for (const auto& month : months)
        *series->add_months() = encode_bar_dataset_request(month);
    } else
      throw std::invalid_argument("invalid factor series");
    // Bars and dominant series are read alike; the market-wide table is not.
    const auto& added = result.series(result.series_size() - 1);
    if (added.has_tables() != result.series(0).has_tables())
      throw std::invalid_argument("factor series must be of one kind and one period");
  }
  if (input.contains("liquidity")) {
    const auto& source = input.at("liquidity");
    require_fields(source, {"minimum_history", "minimum_volume", "minimum_open_interest"});
    if (!source.at("minimum_history").is_number_integer() || source.at("minimum_history") < 1 ||
        source.at("minimum_history") > 10000)
      throw std::invalid_argument("invalid liquidity eligibility rule");
    auto* rule = result.mutable_liquidity();
    rule->set_minimum_history(source.at("minimum_history").get<unsigned>());
    const auto lots = [&](const char* key) {
      if (!source.at(key).is_string())
        throw std::invalid_argument("invalid liquidity eligibility rule");
      const auto text = source.at(key).get<std::string>();
      std::int64_t value = 0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (text.empty() || text.front() == '-' || error != std::errc{} ||
          end != text.data() + text.size() || value < 0)
        throw std::invalid_argument("invalid liquidity eligibility rule");
      return value;
    };
    rule->set_minimum_volume(lots("minimum_volume"));
    rule->set_minimum_open_interest(lots("minimum_open_interest"));
  }
  (void)factor_input(result);
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
  else if (source.has_dominant())
    *query.mutable_dominant_series() = source.dominant();
  else if (source.has_tables())
    *query.mutable_table_series() = source.tables();
  else
    throw std::invalid_argument("invalid factor series");
  return query;
}
std::vector<factor::v1::FactorSeries> factor_series(const factor::v1::FactorSeriesRequest& source,
                                                    data::v1::DataResponse reply) {
  std::vector<factor::v1::FactorSeries> result;
  if (source.has_tables() && reply.has_table_series()) {
    // One series for each instrument asked for, in that order.
    auto& group = *reply.mutable_table_series();
    if (group.series_size() != source.tables().instruments_size())
      throw std::invalid_argument("data reply does not match the requested factor series");
    for (int i = 0; i < group.series_size(); ++i) {
      if (group.series(i).instrument() != source.tables().instruments(i))
        throw std::invalid_argument("data reply does not match the requested factor series");
      *result.emplace_back().mutable_table() = std::move(*group.mutable_series(i));
    }
    return result;
  }
  auto& one = result.emplace_back();
  if (source.has_bars() && reply.has_bar_dataset())
    *one.mutable_bars() = std::move(*reply.mutable_bar_dataset());
  else if (source.has_dominant() && reply.has_dominant_series()) {
    // Data returns the months that are dominant at some point, in roll order.
    auto& resolved = *reply.mutable_dominant_series();
    *one.mutable_dominant()->mutable_months() = std::move(*resolved.mutable_datasets());
    *one.mutable_dominant()->mutable_schedule() = std::move(*resolved.mutable_schedule());
  } else
    throw std::invalid_argument("data reply does not match the requested factor series");
  return result;
}
factor::v1::FactorRequest reuse_daily_factor(const Json& experiment, const std::string& snapshot,
                                             const Json& lookbacks) {
  if (experiment.at("version") != 9)
    throw std::invalid_argument("unsupported factor input version");
  if (!snapshot.empty() && !digest(snapshot))
    throw std::invalid_argument("invalid table snapshot");
  if (!lookbacks.is_null() && lookbacks != experiment.at("lookbacks") &&
      (experiment.at("factor") != "momentum" ||
       experiment.at("evaluation").at("mode") != "full_sample" ||
       experiment.at("lookbacks").size() != 1 || !lookbacks.is_array() || lookbacks.size() != 1))
    throw std::invalid_argument(
        "parameter derivation requires a single-window full-sample daily momentum experiment");
  Json sources = Json::array();
  for (const auto& series : experiment.at("series")) {
    if (series.at("kind") != "table")
      throw std::invalid_argument("experiment reuse currently requires daily table factor inputs");
    const auto& data = series.at("data");
    Json selection{{"snapshot", snapshot.empty() ? data.at("snapshot") : Json(snapshot)},
                   {"daily_source", data.at("daily_source")},
                   {"calendar_source", data.at("calendar_source")},
                   {"begin_day", data.at("begin_day")},
                   {"end_day", data.at("end_day")}};
    bool same = !sources.empty();
    if (same)
      for (const auto& [key, value] : selection.items())
        same = same && sources.back().at("tables").at(key) == value;
    if (same)
      sources.back()["tables"]["instruments"].push_back(data.at("instrument"));
    else {
      selection["instruments"] = Json::array({data.at("instrument")});
      sources.push_back({{"tables", std::move(selection)}});
    }
  }
  Json parameters{{"series", std::move(sources)},
                  {"factor", experiment.at("factor")},
                  {"lookbacks", lookbacks.is_null() ? experiment.at("lookbacks") : lookbacks},
                  {"horizon", experiment.at("horizon")},
                  {"evaluation", experiment.at("evaluation")}};
  if (!experiment.at("liquidity").is_null())
    parameters["liquidity"] = experiment.at("liquidity");
  auto result = encode_factor_request(parameters);
  if (snapshot.empty())
    result.set_expected_dataset_revision(experiment.at("dataset_revision").get<std::string>());
  return result;
}
factor::v1::FactorInput factor_input(const factor::v1::FactorRequest& request) {
  if (!request.expected_dataset_revision().empty() && !digest(request.expected_dataset_revision()))
    throw std::invalid_argument("invalid fixed experiment data revision");
  // An entry of the market-wide table asks for one series per instrument.
  int count = 0;
  for (const auto& source : request.series())
    count += source.has_tables() ? source.tables().instruments_size() : 1;
  if (count != 1 && (count < 3 || count > max_factor_series))
    throw std::invalid_argument("factor analysis studies one series or 3..100 contracts");
  factor::v1::FactorInput input;
  input.set_version(10);
  if (request.has_liquidity()) {
    validate_liquidity(request.liquidity());
    if (count < 3 || std::ranges::any_of(
                         request.series(), [](const auto& series) { return !series.has_tables(); }))
      throw std::invalid_argument("liquidity filtering requires a daily cross-section");
    *input.mutable_liquidity() = request.liquidity();
  }
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
    else if (data.has_table())
      revisions.push_back(table_series_revision(data.table()));
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
namespace {
FactorObservations read_observations(const factor::v1::FactorInput& input, v1::Factor kind) {
  validate_series(input.series());
  std::vector<Observed> all;
  std::set<std::int64_t> times;
  for (const auto& data : input.series()) {
    all.push_back(observed(data, kind));
    const auto& order = all.back().order;
    if (std::ranges::adjacent_find(order, std::ranges::greater_equal()) != order.end())
      throw std::invalid_argument("factor observations must be in ascending order");
    times.insert(order.begin(), order.end());
  }
  const bool terms = kind == v1::TERM_STRUCTURE;
  FactorObservations result;
  result.order.assign(times.begin(), times.end());
  result.closes.resize(all.size());
  result.terms.resize(terms ? all.size() : 0);
  const bool daily = input.series(0).has_table();
  if (daily) {
    result.volumes.resize(all.size());
    result.open_interests.resize(all.size());
  }
  for (std::size_t s = 0; s < all.size(); ++s) {
    // Each series ascends, so one pass places what it has.
    result.closes[s].resize(result.order.size());
    if (terms)
      result.terms[s].resize(result.order.size());
    if (daily) {
      result.volumes[s].resize(result.order.size());
      result.open_interests[s].resize(result.order.size());
      for (const auto& month : input.series(static_cast<int>(s)).table().months())
        for (const auto& close : month.closes()) {
          const auto when = std::chrono::sys_days(parse_trading_date(close.trading_day()))
                                .time_since_epoch()
                                .count();
          const auto found = std::ranges::lower_bound(result.order, when);
          if (found == result.order.end() || *found != when)
            continue; // Term observations may omit a day without a term point.
          const auto index = static_cast<std::size_t>(found - result.order.begin());
          if (close.has_volume_contracts())
            result.volumes[s][index] = close.volume_contracts();
          if (close.has_open_interest_contracts())
            result.open_interests[s][index] = close.open_interest_contracts();
        }
    }
    std::size_t at = 0;
    for (std::size_t i = 0; i < all[s].order.size(); ++i) {
      while (result.order[at] != all[s].order[i])
        ++at;
      result.closes[s][at] = all[s].closes[i];
      if (terms)
        result.terms[s][at] = all[s].terms[i];
    }
  }
  return result;
}
} // namespace
FactorObservations factor_observations(const factor::v1::FactorInput& input) {
  return read_observations(input, input.factor());
}
FactorObservations factor_daily_observations(const factor::v1::FactorInput& input) {
  if (std::ranges::any_of(input.series(), [](const auto& series) { return !series.has_table(); }))
    throw std::invalid_argument("liquidity filtering requires a daily cross-section");
  return read_observations(input, v1::PRICE_MOMENTUM);
}
namespace {
Json factor_definition(const factor::v1::FactorInput& input) {
  validate_message(input);
  if (input.version() != 10)
    throw std::invalid_argument("unsupported factor input version");
  if (input.has_liquidity()) {
    validate_liquidity(input.liquidity());
    if (input.series_size() < 3 ||
        std::ranges::any_of(input.series(), [](const auto& series) { return !series.has_table(); }))
      throw std::invalid_argument("liquidity filtering requires a daily cross-section");
  }
  const std::string factor = factor_name(input.factor());
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
  // A standard deviation needs two returns.
  if (input.factor() == v1::VOLATILITY && input.lookbacks(0) < 2)
    throw std::invalid_argument("volatility lookback must be 2..10000 observations");
  if (input.lookbacks_size() > 1 && !input.has_holdout_start() && !input.has_walk_forward())
    throw std::invalid_argument("factor parameter comparison requires a holdout");
  Json result{
      {"version", 9},
      {"factor", factor},
      {"dataset_revision", input.dataset_revision()},
      {"lookbacks", windows},
      {"horizon", input.horizon()},
      {"liquidity", input.has_liquidity() ? liquidity_definition(input.liquidity()) : Json()}};
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
  if (result.version() != 9)
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
    if (row.series() < 3 && (row.has_pearson() || row.has_spearman()))
      throw std::invalid_argument("invalid correlation result");
  }
  Json partitions = Json::array();
  for (const auto& p : result.partitions()) {
    if (p.name() != "full_sample" && p.name() != "development" && p.name() != "holdout")
      throw std::invalid_argument("invalid factor partition");
    if (p.begin_index() >= p.end_index() || p.end_index() > result.input_count() ||
        p.sample_count() > p.end_index() - p.begin_index())
      throw std::invalid_argument("invalid factor partition range");
    Json spreads = Json::array();
    for (const auto& spread : p.spreads()) {
      if (!std::isfinite(spread.highest()) || !std::isfinite(spread.lowest()) ||
          spread.observations() > p.sample_count() ||
          (!spread.observations() && (spread.highest() != 0 || spread.lowest() != 0)))
        throw std::invalid_argument("non-finite factor result");
      spreads.push_back({{"highest", spread.highest()},
                         {"lowest", spread.lowest()},
                         {"observations", spread.observations()}});
    }
    partitions.push_back({{"name", p.name()},
                          {"spreads", std::move(spreads)},
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
      metadata.set_version(9);
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
  return {{"version", 7},
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
      contracts.insert(data.table().instrument());
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
  if (result.engine_version() != factor_engine_version)
    throw std::invalid_argument("unsupported factor result version");
  const auto reject = [] {
    throw std::invalid_argument("factor result evidence does not match its input");
  };
  std::map<std::string, unsigned> eligible;
  int membership = 0;
  if (input.has_liquidity()) {
    for (int series = 0; series < input.series_size(); ++series)
      for (const auto& month : input.series(series).table().months())
        for (const auto& close : month.closes()) {
          if (membership == result.memberships_size())
            reject();
          const auto& row = result.memberships(membership++);
          if (row.series_index() != static_cast<unsigned>(series) ||
              row.trading_day() != close.trading_day() ||
              !factor::v1::MembershipReason_IsValid(row.reason()) ||
              row.reason() == factor::v1::MEMBERSHIP_UNSPECIFIED ||
              row.reason() == factor::v1::MEMBERSHIP_UNOBSERVED)
            reject();
          eligible[row.trading_day()] += row.reason() == factor::v1::MEMBERSHIP_ELIGIBLE;
        }
  }
  if (membership != result.memberships_size())
    reject();
  const auto order = factor_observations(input).order;
  for (const auto& row : result.cross_sections()) {
    if (row.event_index() >= order.size() ||
        row.series() > static_cast<unsigned>(input.series_size()))
      reject();
    if (input.has_liquidity() && row.series() > eligible[format_trading_date(std::chrono::sys_days{
                                                    std::chrono::days{order[row.event_index()]}})])
      reject();
  }
  const auto count = observation_count(input);
  const auto warmup = input.lookbacks(input.lookbacks_size() - 1);
  const auto horizon = input.horizon();
  const bool search = input.lookbacks_size() > 1;
  if (result.dataset_revision() != input.dataset_revision() || result.input_count() != count ||
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
        (input.series_size() == 1 && (part.has_pearson_ratio() || part.has_spearman_ratio())) ||
        part.spreads_size() != (input.series_size() == 1 ? 0 : input.series_size() / 2))
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
    if (data.has_table()) {
      result["series"].push_back({{"kind", "table"}, {"data", decode_table_series(data.table())}});
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
    }
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
    return input.series(0).has_table()
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
                        {"series", row.series()},
                        {"pearson", row.has_pearson() ? Json(row.pearson()) : Json(nullptr)},
                        {"spearman", row.has_spearman() ? Json(row.spearman()) : Json(nullptr)}});
  Json memberships = Json::array();
  for (const auto& row : result.memberships())
    memberships.push_back({{"series_index", row.series_index()},
                           {"trading_day", row.trading_day()},
                           {"reason", factor::v1::MembershipReason_Name(row.reason())}});
  decoded["memberships"] = std::move(memberships);
  decoded["samples"] = std::move(samples);
  decoded["cross_sections"] = std::move(sections);
  return decoded;
}
} // namespace asterion::protocol
