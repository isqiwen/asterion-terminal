#include "minutes.hpp"
#include "macd.hpp"
#include "tushare.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <map>
#include <optional>
namespace asterion::data_pipeline {
namespace {
constexpr std::int64_t second = 1000000000;
constexpr std::int64_t day = 86400 * second;
constexpr std::size_t limit = 8 * 1024 * 1024;
const char* manifest_name = "minutes.json";
void directory_check(const std::filesystem::path& directory) {
  if (!directory.is_absolute() || !std::filesystem::is_directory(directory) ||
      std::filesystem::is_symlink(directory))
    throw std::invalid_argument("minute dataset requires an existing absolute dedicated directory");
}
Json read_json(const std::filesystem::path& path) {
  if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) ||
      std::filesystem::file_size(path) > limit)
    throw std::invalid_argument("invalid minute dataset file");
  std::ifstream file(path, std::ios::binary);
  std::string bytes(limit + 1, '\0');
  file.read(bytes.data(), bytes.size());
  bytes.resize(file.gcount());
  if (file.bad() || !file.eof() || bytes.size() > limit)
    throw std::runtime_error("cannot read minute dataset");
  return Json::parse(bytes);
}
Json specification(const HistoricalBarRange& request) {
  return {{"ts_code", tushare::code(request.instrument)},
          {"interval_minutes", request.interval_minutes},
          {"begin_ns", std::to_string(request.begin_ns)},
          {"end_ns", std::to_string(request.end_ns)}};
}
HistoricalBarRange range_of(const Json& spec) {
  const auto integer = [](const Json& j) {
    auto value = j.get<std::string>();
    std::int64_t ns = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), ns);
    if (ec != std::errc{} || end != value.data() + value.size())
      throw std::invalid_argument("invalid minute dataset timestamp");
    return ns;
  };
  if (!spec.at("interval_minutes").is_number_integer() || spec.at("interval_minutes") < 1 ||
      spec.at("interval_minutes") > 60)
    throw std::invalid_argument("invalid minute dataset specification");
  HistoricalBarRange range{tushare::instrument(spec.at("ts_code").get<std::string>()),
                           spec.at("interval_minutes").get<unsigned>(),
                           integer(spec.at("begin_ns")), integer(spec.at("end_ns"))};
  tushare::validate(range);
  if (specification(range) != spec)
    throw std::invalid_argument("invalid minute dataset specification");
  return range;
}
Json encode(const HistoricalBar& b) {
  return Json::array({std::to_string(b.timestamp_ns), b.open.str(), b.high.str(), b.low.str(),
                      b.close.str(), b.volume.str(), b.amount.str(), b.open_interest.str()});
}
HistoricalBar decode(const Json& row) {
  if (!row.is_array() || row.size() != 8)
    throw std::invalid_argument("invalid minute dataset bar");
  const auto text = row.at(0).get<std::string>();
  std::int64_t ns = 0;
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), ns);
  if (ec != std::errc{} || end != text.data() + text.size())
    throw std::invalid_argument("invalid minute dataset timestamp");
  const auto d = [&](unsigned i) { return Decimal::parse(row.at(i).get<std::string>()); };
  HistoricalBar bar{ns, d(1), d(2), d(3), d(4), d(5), d(6), d(7)};
  bar.validate();
  return bar;
}
unsigned total_pages(const HistoricalBarRange& range) {
  if (range.end_ns - range.begin_ns > 20LL * 366 * day)
    throw std::invalid_argument("minute download exceeds 20 years");
  return static_cast<unsigned>((range.end_ns - range.begin_ns) / day + 1);
}
HistoricalBarRange page_range(HistoricalBarRange range, unsigned index) {
  range.begin_ns += day * index;
  range.end_ns = std::min(range.end_ns, range.begin_ns + day - second);
  return range;
}
std::string filename(unsigned i) {
  return "minutes-" + std::to_string(i) + ".json";
}
void validate_page(const Json& page, const HistoricalBarRange& range,
                   const std::function<void(const HistoricalBar&)>& consume = {}) {
  if (page.at("version") != 1 || page.at("source") != "tushare.ft_mins" ||
      page.at("request") != specification(range) || !page.at("bars").is_array() ||
      page.at("bars").size() >= 8000)
    throw std::invalid_argument("minute dataset page identity mismatch");
  std::int64_t previous = -1;
  for (const auto& row : page.at("bars")) {
    const auto bar = decode(row);
    if (bar.timestamp_ns <= previous || bar.timestamp_ns < range.begin_ns ||
        bar.timestamp_ns > range.end_ns || bar.timestamp_ns % (60 * second) ||
        bar.volume.raw() % 100000000)
      throw std::invalid_argument("invalid minute dataset order, interval or volume");
    previous = bar.timestamp_ns;
    if (consume)
      consume(bar);
  }
}
Json verify(const std::filesystem::path& dir,
            const std::function<void(const HistoricalBar&)>& consume = {}) {
  const auto manifest = read_json(dir / manifest_name);
  if (manifest.at("version") != 1 || manifest.at("source") != "tushare.ft_mins" ||
      manifest.at("timezone") != "Asia/Shanghai" ||
      manifest.at("timestamp_semantics") != "provider_trade_time" ||
      !manifest.at("pages").is_array())
    throw std::invalid_argument("unsupported minute dataset format");
  const auto range = range_of(manifest.at("request"));
  const auto total = total_pages(range);
  if (manifest.at("pages").size() > total ||
      manifest.at("complete") != (manifest.at("pages").size() == total))
    throw std::invalid_argument("invalid minute dataset coverage");
  if (consume && !manifest.at("complete").get<bool>())
    throw std::invalid_argument("minute dataset is incomplete");
  std::uint64_t rows = 0;
  for (unsigned i = 0; i < manifest.at("pages").size(); ++i) {
    const auto page = read_json(dir / filename(i));
    const auto& entry = manifest.at("pages").at(i);
    if (entry.at("sha256") != sha256_bytes(page.dump()) ||
        entry.at("rows") != page.at("bars").size())
      throw std::invalid_argument("minute dataset digest mismatch");
    validate_page(page, page_range(range, i), consume);
    rows += page.at("bars").size();
  }
  if (manifest.at("rows") != rows)
    throw std::invalid_argument("minute dataset row count mismatch");
  return manifest;
}
} // namespace
HistoricalBarRange minute_range(const data::v1::MinuteDownload& input) {
  if (input.version() != 1 || input.requests_per_minute() < 1 || input.requests_per_minute() > 500)
    throw std::invalid_argument("invalid minute download definition");
  HistoricalBarRange range{tushare::instrument(input.ts_code()), input.interval_minutes(),
                           input.begin_ns(), input.end_ns()};
  tushare::validate(range);
  (void)total_pages(range);
  return range;
}
Json minute_request_json(const data::v1::MinuteDownload& input) {
  auto result = specification(minute_range(input));
  result["version"] = input.version();
  result["requests_per_minute"] = input.requests_per_minute();
  return result;
}
data::v1::MinuteDownload minute_request(const Json& value) {
  require_fields(value, {"version", "ts_code", "interval_minutes", "begin_ns", "end_ns",
                         "requests_per_minute"});
  if (!value.at("version").is_number_integer() || value.at("version") != 1 ||
      !value.at("requests_per_minute").is_number_integer() || value.at("requests_per_minute") < 1 ||
      value.at("requests_per_minute") > 500)
    throw std::invalid_argument("invalid minute download definition");
  auto spec = value;
  spec.erase("version");
  spec.erase("requests_per_minute");
  const auto range = range_of(spec);
  data::v1::MinuteDownload input;
  input.set_version(value.at("version").get<unsigned>());
  input.set_ts_code(value.at("ts_code").get<std::string>());
  input.set_interval_minutes(range.interval_minutes);
  input.set_begin_ns(range.begin_ns);
  input.set_end_ns(range.end_ns);
  input.set_requests_per_minute(value.at("requests_per_minute").get<unsigned>());
  (void)minute_range(input);
  return input;
}
data::v1::MinuteDownloadResult minute_result(const std::filesystem::path& dir) {
  auto manifest = inspect_minutes(dir);
  if (!manifest.at("complete").get<bool>())
    throw std::invalid_argument("minute dataset is incomplete");
  data::v1::MinuteDownloadResult result;
  result.set_version(1);
  const auto path = dir.u8string();
  result.set_directory(std::string(path.begin(), path.end()));
  result.set_manifest_sha256(sha256_bytes(manifest.dump()));
  result.set_rows(manifest.at("rows").get<std::uint64_t>());
  result.set_pages(static_cast<unsigned>(manifest.at("pages").size()));
  return result;
}
void verify_minute_result(const data::v1::MinuteDownload& input,
                          const data::v1::MinuteDownloadResult& result) {
  const auto directory =
      std::filesystem::path(std::u8string(result.directory().begin(), result.directory().end()));
  const auto manifest = inspect_minutes(directory);
  if (manifest.at("request") != specification(minute_range(input)) ||
      minute_result(directory).SerializeAsString() != result.SerializeAsString())
    throw std::invalid_argument("minute dataset result does not match request");
}
Json download_minutes(HistoricalBarPort& provider, const HistoricalBarRange& range,
                      const std::filesystem::path& directory, unsigned rpm, std::stop_token stop,
                      DownloadProgress progress) {
  if (provider.descriptor().id != "asterion.data.tushare")
    throw std::invalid_argument("minute download requires the Tushare data plugin");
  tushare::validate(range);
  const auto total = total_pages(range);
  if (rpm == 0 || rpm > 500)
    throw std::invalid_argument("Tushare request rate must be 1..500 per minute");
  directory_check(directory);
  FileLock lock(directory, "minutes.lock");
  Json manifest;
  if (std::filesystem::exists(directory / manifest_name)) {
    manifest = verify(directory);
    if (manifest.at("request") != specification(range))
      throw std::invalid_argument("minute dataset request differs; choose a new directory");
  } else {
    for (const auto& file : std::filesystem::directory_iterator(directory))
      if (file.path().filename() != "minutes.lock")
        throw std::invalid_argument("minute dataset directory is not empty");
    manifest = {{"version", 1},
                {"source", "tushare.ft_mins"},
                {"timezone", "Asia/Shanghai"},
                {"timestamp_semantics", "provider_trade_time"},
                {"request", specification(range)},
                {"pages", Json::array()},
                {"rows", 0},
                {"complete", false}};
    replace_file_durably(directory / manifest_name, manifest.dump());
  }
  std::uint64_t rows = manifest.at("rows").get<std::uint64_t>();
  auto next_request = std::chrono::steady_clock::now();
  if (progress)
    progress(static_cast<unsigned>(manifest.at("pages").size()), total, rows);
  for (unsigned i = static_cast<unsigned>(manifest.at("pages").size()); i < total; ++i) {
    if (stop.stop_requested())
      throw std::runtime_error("Tushare download cancelled");
    auto request = page_range(range, i);
    const auto path = directory / filename(i);
    Json page;
    if (std::filesystem::exists(path)) {
      // Recovery of the page durably written immediately before a crash that
      // prevented advancing the manifest. A different page is never replaced.
      page = read_json(path);
      validate_page(page, request);
    } else {
      std::mutex mutex;
      std::condition_variable_any changed;
      std::unique_lock guard(mutex);
      changed.wait_until(guard, stop, next_request, [] { return false; });
      if (stop.stop_requested())
        throw std::runtime_error("Tushare download cancelled");
      next_request =
          std::chrono::steady_clock::now() + std::chrono::milliseconds((60000 + rpm - 1) / rpm);
      const auto bars = provider.read(request, stop);
      Json encoded = Json::array();
      for (const auto& bar : bars)
        encoded.push_back(encode(bar));
      page = {{"version", 1},
              {"source", "tushare.ft_mins"},
              {"request", specification(request)},
              {"bars", std::move(encoded)}};
      validate_page(page, request);
      replace_file_durably(path, page.dump());
    }
    rows += page.at("bars").size();
    manifest["pages"].push_back(
        {{"rows", page.at("bars").size()}, {"sha256", sha256_bytes(page.dump())}});
    manifest["rows"] = rows;
    manifest["complete"] = i + 1 == total;
    replace_file_durably(directory / manifest_name, manifest.dump());
    if (progress)
      progress(i + 1, total, rows);
  }
  return manifest;
}
Json inspect_minutes(const std::filesystem::path& dir) {
  directory_check(dir);
  FileLock lock(dir, "minutes.lock", FileLock::Access::shared);
  return verify(dir);
}
void read_minutes(const std::filesystem::path& dir,
                  const std::function<void(const HistoricalBar&)>& consume) {
  directory_check(dir);
  FileLock lock(dir, "minutes.lock", FileLock::Access::shared);
  // Validate the full immutable input before delivering any bar to a consumer.
  const auto manifest = verify(dir);
  if (!manifest.at("complete").get<bool>())
    throw std::invalid_argument("minute dataset is incomplete");
  (void)verify(dir, consume);
}

data::v1::MinutePage read_minute_page(const data::v1::MinuteDownload& input,
                                      const data::v1::MinuteDownloadResult& result,
                                      const data::v1::MinutePageQuery& query) {
  if (query.task_id().empty() || query.limit() < 1 || query.limit() > 200 || query.begin_ns() < 0 ||
      query.end_ns() < 0)
    throw std::invalid_argument("invalid minute dataset page query");
  const auto range = minute_range(input);
  const auto begin = query.begin_ns() ? query.begin_ns() : range.begin_ns;
  const auto end = query.end_ns() ? query.end_ns() : range.end_ns;
  if (begin > end)
    throw std::invalid_argument("invalid minute dataset page query");
  const auto dir =
      std::filesystem::path(std::u8string(result.directory().begin(), result.directory().end()));
  directory_check(dir);
  FileLock lock(dir, "minutes.lock", FileLock::Access::shared);
  const auto manifest = read_json(dir / manifest_name);
  if (result.version() != 1 || sha256_bytes(manifest.dump()) != result.manifest_sha256() ||
      manifest.at("request") != specification(range) || !manifest.at("complete").get<bool>() ||
      manifest.at("pages").size() != total_pages(range) || manifest.at("rows") != result.rows())
    throw std::invalid_argument("minute dataset result does not match request");
  std::map<unsigned, std::vector<HistoricalBar>> loaded;
  const auto load = [&](unsigned index) -> const std::vector<HistoricalBar>& {
    if (loaded.contains(index))
      return loaded.at(index);
    const auto page = read_json(dir / filename(index));
    const auto& entry = manifest.at("pages").at(index);
    if (entry.at("sha256") != sha256_bytes(page.dump()) ||
        entry.at("rows") != page.at("bars").size())
      throw std::invalid_argument("minute dataset digest mismatch");
    std::vector<HistoricalBar> rows;
    validate_page(page, page_range(range, index), [&](const auto& bar) { rows.push_back(bar); });
    return loaded.emplace(index, std::move(rows)).first->second;
  };
  data::v1::MinutePage output;
  output.set_version(1);
  output.set_task_id(query.task_id());
  output.set_source("tushare.ft_mins");
  output.set_ts_code(input.ts_code());
  output.set_interval_minutes(input.interval_minutes());
  output.set_manifest_sha256(result.manifest_sha256());
  output.set_total_rows(result.rows());
  output.set_offset(query.offset());
  output.set_limit(query.limit());
  output.set_begin_ns(begin);
  output.set_end_ns(end);
  std::vector<std::uint64_t> counts;
  std::uint64_t total = 0, matched = 0;
  std::optional<unsigned> first, last;
  for (unsigned i = 0; i < total_pages(range); ++i) {
    const auto size = manifest.at("pages").at(i).at("rows").get<std::uint64_t>();
    if (size >= 8000)
      throw std::invalid_argument("minute dataset row count mismatch");
    total += size;
    if (size) {
      if (!first)
        first = i;
      last = i;
    }
    const auto span = page_range(range, i);
    std::uint64_t count = 0;
    if (size && span.end_ns >= begin && span.begin_ns <= end) {
      if (span.begin_ns >= begin && span.end_ns <= end)
        count = size;
      else
        for (const auto& bar : load(i))
          if (bar.timestamp_ns >= begin && bar.timestamp_ns <= end)
            ++count;
    }
    counts.push_back(count);
    matched += count;
  }
  if (total != result.rows())
    throw std::invalid_argument("minute dataset row count mismatch");
  output.set_matched_rows(matched);
  if (query.offset() > matched || (matched && query.offset() == matched))
    throw std::invalid_argument("minute dataset page offset is out of range");
  if (first) {
    output.set_first_ns(load(*first).front().timestamp_ns);
    output.set_last_ns(load(*last).back().timestamp_ns);
  }
  // Coverage and intersecting boundary chunks have been verified. Without
  // output bars there are no indicators to compute, so no prefix scan is needed.
  if (!matched)
    return output;
  chart_indicators::Macd macd;
  std::uint64_t skip = query.offset();
  for (unsigned i = 0; i < counts.size() && output.bars_size() < static_cast<int>(query.limit());
       ++i) {
    if (page_range(range, i).begin_ns > end)
      break;
    if (!query.include_macd() && skip >= counts[i]) {
      skip -= counts[i];
      continue;
    }
    for (const auto& bar : load(i)) {
      if (bar.timestamp_ns > end)
        break;
      const auto indicator = query.include_macd() ? macd.push(bar.close) : std::nullopt;
      if (bar.timestamp_ns < begin || bar.timestamp_ns > end)
        continue;
      if (skip) {
        --skip;
        continue;
      }
      if (output.bars_size() == static_cast<int>(query.limit()))
        break;
      auto* row = output.add_bars();
      row->set_timestamp_ns(bar.timestamp_ns);
      row->mutable_open()->set_units(bar.open.raw());
      row->mutable_high()->set_units(bar.high.raw());
      row->mutable_low()->set_units(bar.low.raw());
      row->mutable_close()->set_units(bar.close.raw());
      row->mutable_volume()->set_units(bar.volume.raw());
      row->mutable_amount()->set_units(bar.amount.raw());
      row->mutable_open_interest()->set_units(bar.open_interest.raw());
      if (indicator) {
        row->mutable_macd()->set_diff(indicator->diff);
        row->mutable_macd()->set_signal(indicator->signal);
        row->mutable_macd()->set_histogram(indicator->histogram);
      }
    }
    loaded.erase(i); // Prefix scans retain only one validated source segment.
  }
  return output;
}
} // namespace asterion::data_pipeline
