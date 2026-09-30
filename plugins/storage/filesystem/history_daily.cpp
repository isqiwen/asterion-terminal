#include <asterion/protocol/data.hpp>
#include "history_daily.hpp"
#include "history_metadata.hpp"
#include "bar_parquet.hpp"
#include "macd.hpp"
#include "calendar_bars.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <condition_variable>
#include <fstream>
#include <mutex>
namespace asterion::history_files {
namespace {
using namespace std::chrono;

constexpr auto manifest_name = "daily.json";
void directory_check(const std::filesystem::path& dir) {
  if (!dir.is_absolute() || !std::filesystem::is_directory(dir) || std::filesystem::is_symlink(dir))
    throw std::invalid_argument("daily dataset requires an existing absolute dedicated directory");
}
unsigned page_count(const HistoricalDailyRange& range) {
  (void)range.instrument.key();
  validate_history_source(range.source);
  (void)format_trading_date(range.begin);
  (void)format_trading_date(range.end);
  const auto span = (sys_days(range.end) - sys_days(range.begin)).count();
  if (range.begin.year() < year(1990) || range.end.year() > year(2100) || span < 0 ||
      span > 20 * 366)
    throw std::invalid_argument("invalid daily dataset range");
  return static_cast<unsigned>(span / 366 + 1);
}
HistoricalDailyRange page_range(HistoricalDailyRange range, unsigned index) {
  range.begin = year_month_day(sys_days(range.begin) + days(366 * index));
  range.end = year_month_day(std::min(sys_days(range.end), sys_days(range.begin) + days(365)));
  return range;
}
Json specification(const HistoricalDailyRange& range) {
  return {{"contract_id", range.instrument.key()},
          {"source", range.source},
          {"source_instrument", range.source_instrument},
          {"begin_day", format_trading_date(range.begin)},
          {"end_day", format_trading_date(range.end)}};
}
HistoricalDailyRange range_of(const Json& value) {
  require_fields(value, {"contract_id", "source", "source_instrument", "begin_day", "end_day"});
  HistoricalDailyRange range{HistoryIdentity::parse(value.at("contract_id").get<std::string>()),
                             parse_trading_date(value.at("begin_day").get<std::string>()),
                             parse_trading_date(value.at("end_day").get<std::string>()),
                             value.at("source").get<std::string>(),
                             value.at("source_instrument").get<std::string>()};
  (void)page_count(range);
  if (specification(range) != value)
    throw std::invalid_argument("invalid daily dataset range");
  return range;
}
std::string filename(unsigned index) {
  return "daily-" + std::to_string(index) + ".parquet";
}
Json read_json(const std::filesystem::path& path) {
  if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) ||
      std::filesystem::file_size(path) > 8 * 1024 * 1024)
    throw std::invalid_argument("invalid daily dataset file");
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot read daily dataset file");
  return parse_json(std::string(std::istreambuf_iterator<char>(file), {}), 8 * 1024 * 1024);
}
void validate_page(const std::vector<HistoricalDailyBar>& bars, const HistoricalDailyRange& range,
                   std::vector<HistoricalDailyBar>* output = nullptr) {
  if (bars.size() > 366)
    throw std::invalid_argument("invalid daily dataset page");
  std::optional<year_month_day> previous;
  for (const auto& bar : bars) {
    bar.validate();
    if (bar.volume.raw() % 100000000 || bar.open_interest.raw() % 100000000)
      throw std::invalid_argument("invalid daily dataset quantity");
    if (bar.trading_day < range.begin || bar.trading_day > range.end ||
        (previous && bar.trading_day <= *previous))
      throw std::invalid_argument("invalid daily dataset order or coverage");
    previous = bar.trading_day;
    if (output)
      output->push_back(bar);
  }
}
Json verify(const std::filesystem::path& dir, std::vector<HistoricalDailyBar>* output = nullptr) {
  const auto manifest = read_json(dir / manifest_name);
  require_fields(manifest, {"version", "source", "semantics", "amount_unit", "request", "pages",
                            "rows", "complete"});
  if (!manifest.at("version").is_number_integer() || manifest.at("version") != 3 ||
      manifest.at("source") != manifest.at("request").at("source") ||
      !valid_semantics(manifest.at("semantics")) ||
      manifest.at("semantics").at("source") != manifest.at("source") ||
      manifest.at("amount_unit") != "quote_currency" || !manifest.at("pages").is_array() ||
      !manifest.at("complete").is_boolean() || !manifest.at("rows").is_number_unsigned())
    throw std::invalid_argument("unsupported daily dataset format");
  const auto range = range_of(manifest.at("request"));
  const auto count = page_count(range);
  if (manifest.at("pages").size() > count ||
      manifest.at("complete") != (manifest.at("pages").size() == count))
    throw std::invalid_argument("invalid daily dataset coverage");
  if (output && !manifest.at("complete").get<bool>())
    throw std::invalid_argument("daily dataset is incomplete");
  std::uint64_t rows = 0;
  for (unsigned i = 0; i < manifest.at("pages").size(); ++i) {
    const auto path = dir / filename(i);
    const auto& entry = manifest.at("pages").at(i);
    require_fields(entry, {"rows", "sha256"});
    if (!entry.at("rows").is_number_unsigned() || std::filesystem::is_symlink(path) ||
        !std::filesystem::is_regular_file(path) ||
        entry.at("sha256").get<std::string>() != sha256_file(path))
      throw std::invalid_argument("daily dataset digest mismatch");
    const auto page = parquet::read_daily_bars(path);
    if (entry.at("rows") != page.size())
      throw std::invalid_argument("daily dataset digest mismatch");
    validate_page(page, page_range(range, i), output);
    rows += page.size();
  }
  if (manifest.at("rows") != rows)
    throw std::invalid_argument("daily dataset row count mismatch");
  return manifest;
}
DailyDatasetInfo info(const Json& manifest) {
  return {range_of(manifest.at("request")), manifest.at("rows").get<std::uint64_t>(),
          static_cast<unsigned>(manifest.at("pages").size()), manifest.at("complete").get<bool>(),
          sha256_bytes(manifest.dump())};
}
} // namespace
HistoricalDailyRange daily_range(const data::v1::DailyDownload& input) {
  if (input.version() != 2 || input.requests_per_minute() < 1 || input.requests_per_minute() > 500)
    throw std::invalid_argument("invalid daily download definition");
  HistoricalDailyRange range{
      HistoryIdentity::parse(input.contract_id()), parse_trading_date(input.begin_day()),
      parse_trading_date(input.end_day()), input.source(), input.source_instrument()};
  (void)page_count(range);
  return range;
}
unsigned daily_work_units(const data::v1::DailyDownload& input) {
  return page_count(daily_range(input));
}
Json daily_request_json(const data::v1::DailyDownload& input) {
  auto result = specification(daily_range(input));
  result["version"] = input.version();
  result["requests_per_minute"] = input.requests_per_minute();
  return result;
}
data::v1::DailyDownload daily_request(const Json& value) {
  require_fields(value, {"version", "contract_id", "source", "source_instrument", "begin_day",
                         "end_day", "requests_per_minute"});
  if (!value.at("version").is_number_integer() || value.at("version") != 2 ||
      !value.at("requests_per_minute").is_number_integer() || value.at("requests_per_minute") < 1 ||
      value.at("requests_per_minute") > 500)
    throw std::invalid_argument("invalid daily download definition");
  data::v1::DailyDownload input;
  input.set_version(2);
  input.set_contract_id(value.at("contract_id").get<std::string>());
  input.set_source(value.at("source").get<std::string>());
  input.set_source_instrument(value.at("source_instrument").get<std::string>());
  input.set_begin_day(value.at("begin_day").get<std::string>());
  input.set_end_day(value.at("end_day").get<std::string>());
  input.set_requests_per_minute(value.at("requests_per_minute").get<unsigned>());
  (void)daily_range(input);
  return input;
}
data::v1::DailyDownloadResult daily_result(const std::filesystem::path& dir) {
  const auto info = inspect_daily(dir);
  if (!info.complete)
    throw std::invalid_argument("daily dataset is incomplete");
  data::v1::DailyDownloadResult result;
  result.set_version(2);
  const auto path = dir.u8string();
  result.set_directory(std::string(path.begin(), path.end()));
  result.set_manifest_sha256(info.manifest_sha256);
  result.set_rows(info.rows);
  result.set_pages(info.pages);
  return result;
}
void verify_daily_result(const data::v1::DailyDownload& input,
                         const data::v1::DailyDownloadResult& result) {
  const auto directory =
      std::filesystem::path(std::u8string(result.directory().begin(), result.directory().end()));
  const auto info = inspect_daily(directory);
  if (specification(info.range) != specification(daily_range(input)) || !info.complete ||
      result.version() != 2 || result.manifest_sha256() != info.manifest_sha256 ||
      result.rows() != info.rows || result.pages() != info.pages)
    throw std::invalid_argument("daily dataset result does not match request");
}
DailyDatasetInfo download_daily(HistoricalDailyPort& provider, const HistoricalDailyRange& range,
                                const std::filesystem::path& dir, unsigned rpm,
                                std::stop_token stop, DailyProgress progress) {
  const auto semantics = encode_semantics(provider.semantics());
  if (provider.semantics().source != range.source)
    throw std::invalid_argument("historical source mismatch");
  const auto total = page_count(range);
  if (!rpm || rpm > 500)
    throw std::invalid_argument("Historical request rate must be 1..500 per minute");
  directory_check(dir);
  FileLock lock(dir, "daily.lock");
  Json manifest;
  if (std::filesystem::exists(dir / manifest_name)) {
    manifest = verify(dir);
    if (manifest.at("request") != specification(range) || manifest.at("semantics") != semantics)
      throw std::invalid_argument("daily dataset request differs; choose a new directory");
  } else {
    for (const auto& file : std::filesystem::directory_iterator(dir))
      if (file.path().filename() != "daily.lock")
        throw std::invalid_argument("daily dataset directory is not empty");
    manifest = {{"version", 3},
                {"source", range.source},
                {"semantics", semantics},
                {"amount_unit", "quote_currency"},
                {"request", specification(range)},
                {"pages", Json::array()},
                {"rows", 0},
                {"complete", false}};
    replace_file_durably(dir / manifest_name, manifest.dump());
  }
  auto rows = manifest.at("rows").get<std::uint64_t>();
  auto next_request = steady_clock::now();
  if (progress)
    progress(static_cast<unsigned>(manifest.at("pages").size()), total, rows);
  for (unsigned i = static_cast<unsigned>(manifest.at("pages").size()); i < total; ++i) {
    if (stop.stop_requested())
      throw std::runtime_error("Historical download cancelled");
    const auto request = page_range(range, i);
    const auto path = dir / filename(i);
    std::vector<HistoricalDailyBar> page;
    if (std::filesystem::exists(path)) {
      // A page published just before a crash stopped the manifest update.
      page = parquet::read_daily_bars(path);
      validate_page(page, request);
    } else {
      std::mutex mutex;
      std::condition_variable_any wake;
      std::unique_lock guard(mutex);
      wake.wait_until(guard, stop, next_request, [] { return false; });
      if (stop.stop_requested())
        throw std::runtime_error("Historical download cancelled");
      next_request = steady_clock::now() + milliseconds((60000 + rpm - 1) / rpm);
      page = provider.read(request, stop);
      validate_page(page, request);
      parquet::write_daily_bars(path, page);
    }
    rows += page.size();
    manifest["pages"].push_back({{"rows", page.size()}, {"sha256", sha256_file(path)}});
    manifest["rows"] = rows;
    manifest["complete"] = i + 1 == total;
    replace_file_durably(dir / manifest_name, manifest.dump());
    if (progress)
      progress(i + 1, total, rows);
  }
  return info(manifest);
}
std::vector<std::string> daily_trading_days(const std::filesystem::path& dir) {
  directory_check(dir);
  FileLock lock(dir, "daily.lock", FileLock::Access::shared);
  const auto manifest = read_json(dir / manifest_name);
  if (manifest.at("version") != 3 || !manifest.at("complete").get<bool>())
    throw std::invalid_argument("daily dataset is incomplete");
  std::vector<std::filesystem::path> files;
  for (unsigned i = 0; i < manifest.at("pages").size(); ++i) {
    const auto path = dir / filename(i);
    if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) ||
        sha256_file(path) != manifest.at("pages").at(i).at("sha256").get<std::string>())
      throw std::invalid_argument("daily dataset digest mismatch");
    if (manifest.at("pages").at(i).at("rows").get<std::uint64_t>())
      files.push_back(path);
  }
  return parquet::trading_days(files, false);
}
DailyDatasetInfo inspect_daily(const std::filesystem::path& dir) {
  directory_check(dir);
  FileLock lock(dir, "daily.lock", FileLock::Access::shared);
  return info(verify(dir));
}
DailyDataset read_daily(const std::filesystem::path& dir) {
  directory_check(dir);
  FileLock lock(dir, "daily.lock", FileLock::Access::shared);
  DailyDataset result;
  result.info = info(verify(dir, &result.bars));
  return result;
}
data::v1::DailyPage read_daily_page(const data::v1::DailyDownload& input,
                                    const data::v1::DailyDownloadResult& result,
                                    const data::v1::DailyPageQuery& query) {
  validate_id(query.dataset_id().empty() ? query.task_id() : query.dataset_id());
  if (query.limit() < 1 || query.limit() > 200 || !data::v1::DailyPeriod_IsValid(query.period()))
    throw std::invalid_argument("invalid daily dataset page query");
  const auto range = daily_range(input);
  const auto begin =
      query.begin_day().empty() ? range.begin : parse_trading_date(query.begin_day());
  const auto end = query.end_day().empty() ? range.end : parse_trading_date(query.end_day());
  if (begin > end)
    throw std::invalid_argument("invalid daily dataset page query");
  const auto dir =
      std::filesystem::path(std::u8string(result.directory().begin(), result.directory().end()));
  const auto dataset = read_daily(
      dir); // Bounded to twenty years; validates every source segment under one shared lock.
  const auto& meta = dataset.info;
  if (specification(meta.range) != specification(range) || result.version() != 2 ||
      result.manifest_sha256() != meta.manifest_sha256 || result.rows() != meta.rows ||
      result.pages() != meta.pages)
    throw std::invalid_argument("daily dataset result does not match request");
  std::vector<HistoricalDailyBar> grouped;
  if (query.period() != data::v1::DAY) {
    for (auto& bucket : chart_indicators::calendar_bars(
             dataset.bars,
             query.period() == data::v1::WEEK      ? chart_indicators::CalendarPeriod::week
             : query.period() == data::v1::MONTH   ? chart_indicators::CalendarPeriod::month
             : query.period() == data::v1::QUARTER ? chart_indicators::CalendarPeriod::quarter
                                                   : chart_indicators::CalendarPeriod::year))
      grouped.push_back(std::move(bucket.bar));
  }
  const auto& bars = query.period() == data::v1::DAY ? dataset.bars : grouped;
  data::v1::DailyPage page;
  page.set_version(2);
  page.set_task_id(query.dataset_id().empty() ? query.task_id() : query.dataset_id());
  page.set_source(input.source());
  page.set_contract_id(input.contract_id());
  page.set_manifest_sha256(meta.manifest_sha256);
  page.set_period(query.period());
  page.set_total_rows(bars.size());
  page.set_offset(query.offset());
  page.set_limit(query.limit());
  page.set_begin_day(format_trading_date(begin));
  page.set_end_day(format_trading_date(end));
  if (!dataset.bars.empty()) {
    page.set_first_day(format_trading_date(dataset.bars.front().trading_day));
    page.set_last_day(format_trading_date(dataset.bars.back().trading_day));
  }
  std::uint64_t matched = 0;
  for (const auto& bar : bars)
    if (bar.trading_day >= begin && bar.trading_day <= end)
      ++matched;
  page.set_matched_rows(matched);
  if (query.offset() > matched || (matched && query.offset() == matched))
    throw std::invalid_argument("daily dataset page offset is out of range");
  chart_indicators::Macd macd;
  auto skip = query.offset();
  for (const auto& bar : bars) {
    if (bar.trading_day > end || page.bars_size() == static_cast<int>(query.limit()))
      break;
    const auto indicator = query.include_macd() ? macd.push(bar.close) : std::nullopt;
    if (bar.trading_day < begin)
      continue;
    if (skip) {
      --skip;
      continue;
    }
    auto* row = page.add_bars();
    *row = protocol::encode_daily_bar(bar);
    if (indicator) {
      row->mutable_macd()->set_diff(indicator->diff);
      row->mutable_macd()->set_signal(indicator->signal);
      row->mutable_macd()->set_histogram(indicator->histogram);
    }
  }
  return page;
}
} // namespace asterion::history_files
