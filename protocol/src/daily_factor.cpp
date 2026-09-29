#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <stdexcept>
#include <cmath>
#include <array>

namespace asterion::protocol {
research::v1::DailyFactorRequest encode_daily_factor_request(const Json& value) {
  require_fields(value, {"source_task_id", "lookback", "horizon", "evaluation"});
  research::v1::DailyFactorRequest request;
  request.set_source_task_id(value.at("source_task_id").get<std::string>());
  validate_id(request.source_task_id());
  for (const auto* key : {"lookback", "horizon"})
    if (!value.at(key).is_number_integer() || value.at(key) < 1 || value.at(key) > 10000)
      throw std::invalid_argument("invalid daily factor request");
  request.set_lookback(value.at("lookback").get<unsigned>());
  request.set_horizon(value.at("horizon").get<unsigned>());
  const auto& evaluation = value.at("evaluation");
  if (!evaluation.is_object() || !evaluation.contains("mode"))
    throw std::invalid_argument("invalid daily factor request");
  if (evaluation.at("mode") == "full_sample") {
    require_fields(evaluation, {"mode"});
    request.set_full_sample(true);
  } else if (evaluation.at("mode") == "holdout") {
    require_fields(evaluation, {"mode", "split_index"});
    const auto& split = evaluation.at("split_index");
    if (!split.is_number_integer() || split < 1 || split > 7321)
      throw std::invalid_argument("invalid daily factor request");
    request.set_holdout_start(split.get<unsigned>());
  } else
    throw std::invalid_argument("invalid daily factor request");
  return request;
}
Json decode_daily_factor(const research::v1::DailyFactorInput& input,
                         const research::v1::DailyFactorResult& result) {
  validate_daily_factor(input);
  validate_message(result);
  const auto reject = [] { throw std::invalid_argument("invalid daily factor result evidence"); };
  const auto count = static_cast<unsigned>(input.dataset().bars_size());
  const auto split = input.has_holdout_start() ? input.holdout_start() : count;
  if (result.version() != 1 ||
      result.engine_version() != "asterion.factor.daily-close-momentum.v1" ||
      result.dataset_revision() != input.dataset_revision() || result.input_count() != count ||
      result.lookback() != input.lookback() || result.horizon() != input.horizon())
    reject();
  Json samples = Json::array(), partitions = Json::array();
  unsigned sample_index = 0, purged = 0;
  std::array<unsigned, 2> sizes{};
  for (unsigned index = input.lookback(); index + input.horizon() < count; ++index) {
    if (index < split && index + input.horizon() >= split) {
      ++purged;
      continue;
    }
    if (sample_index >= static_cast<unsigned>(result.samples_size()))
      reject();
    const auto& row = result.samples(static_cast<int>(sample_index++));
    if (row.observation_index() != index ||
        row.trading_day() != input.dataset().bars(static_cast<int>(index)).trading_day() ||
        row.label_day() !=
            input.dataset().bars(static_cast<int>(index + input.horizon())).trading_day() ||
        !std::isfinite(row.value()) || !std::isfinite(row.forward_return()))
      reject();
    ++sizes[index < split ? 0 : 1];
    samples.push_back({{"observation_index", index},
                       {"trading_day", row.trading_day()},
                       {"label_day", row.label_day()},
                       {"value", row.value()},
                       {"forward_return", row.forward_return()}});
  }
  if (sample_index != static_cast<unsigned>(result.samples_size()) ||
      result.purged_count() != purged ||
      result.partitions_size() != (input.has_holdout_start() ? 2 : 1))
    reject();
  for (int i = 0; i < result.partitions_size(); ++i) {
    const auto& part = result.partitions(i);
    const std::string name = !input.has_holdout_start() ? "full_sample"
                             : i                        ? "holdout"
                                                        : "development";
    if (part.name() != name || part.begin_index() != (i ? split : 0) ||
        part.end_index() != (i ? count : split) || part.sample_count() != sizes[i] ||
        (part.has_pearson() && (!std::isfinite(part.pearson()) || std::abs(part.pearson()) > 1)) ||
        (part.has_spearman() && (!std::isfinite(part.spearman()) || std::abs(part.spearman()) > 1)))
      reject();
    partitions.push_back(
        {{"name", name},
         {"begin_index", part.begin_index()},
         {"end_index", part.end_index()},
         {"sample_count", part.sample_count()},
         {"pearson", part.has_pearson() ? Json(part.pearson()) : Json(nullptr)},
         {"spearman", part.has_spearman() ? Json(part.spearman()) : Json(nullptr)}});
  }
  const auto& data = input.dataset();
  return {
      {"experiment",
       {{"version", input.version()},
        {"dataset_revision", input.dataset_revision()},
        {"lookback", input.lookback()},
        {"horizon", input.horizon()},
        {"evaluation", input.has_holdout_start() ? Json{{"mode", "holdout"}, {"split_index", split}}
                                                 : Json{{"mode", "full_sample"}}},
        {"data",
         {{"source_task_id", data.source_task_id()},
          {"source", data.source()},
          {"ts_code", data.ts_code()},
          {"manifest_sha256", data.manifest_sha256()},
          {"count", count},
          {"first_day", data.bars(0).trading_day()},
          {"last_day", data.bars(static_cast<int>(count - 1)).trading_day()}}}}},
      {"result",
       {{"version", result.version()},
        {"engine_version", result.engine_version()},
        {"dataset_revision", result.dataset_revision()},
        {"input_count", count},
        {"purged_count", purged},
        {"samples", samples},
        {"partitions", partitions}}}};
}
std::vector<HistoricalDailyBar> daily_factor_bars(const research::v1::DailyFactorDataset& input) {
  validate_message(input);
  validate_id(input.source_task_id());
  if (input.version() != 1 || input.source() != "tushare.fut_daily" || input.ts_code().empty() ||
      input.ts_code().size() > 64 ||
      input.ts_code().find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.") !=
          std::string::npos ||
      input.manifest_sha256().size() != 64 ||
      input.manifest_sha256().find_first_not_of("0123456789abcdef") != std::string::npos ||
      input.bars_size() < 1 || input.bars_size() > 20 * 366 + 1)
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
std::string daily_factor_revision(const research::v1::DailyFactorDataset& input) {
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
      {"source_task_id", input.source_task_id()},
      {"source", input.source()},
      {"ts_code", input.ts_code()},
      {"manifest_sha256", input.manifest_sha256()},
      {"bars", rows}}.dump());
}
void validate_daily_factor(const research::v1::DailyFactorInput& input) {
  validate_message(input);
  if (input.version() != 1 || !input.has_dataset() || !input.lookback() ||
      input.lookback() > 10000 || !input.horizon() || input.horizon() > 10000)
    throw std::invalid_argument("invalid daily factor input");
  if (input.dataset_revision() != daily_factor_revision(input.dataset()))
    throw std::invalid_argument("daily factor dataset revision mismatch");
  const auto count = static_cast<unsigned>(input.dataset().bars_size());
  if (count < input.lookback() + input.horizon() + 30)
    throw std::invalid_argument("daily factor requires at least 30 labelled observations");
  if (input.has_full_sample() && input.full_sample())
    return;
  if (input.has_holdout_start() &&
      input.holdout_start() >= input.lookback() + input.horizon() + 30 &&
      input.holdout_start() < count && count - input.holdout_start() >= input.horizon() + 30)
    return;
  throw std::invalid_argument("invalid daily factor evaluation boundary");
}
} // namespace asterion::protocol
