#include "history_minutes.hpp"
#include "macd.hpp"
#include "history_metadata.hpp"
#include "bar_parquet.hpp"
#include <asterion/domain/daily_bars.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <chrono>
#include <fstream>
#include <mutex>
#include <map>
#include <optional>
#include <list>
#include <algorithm>
namespace asterion::history_files {
namespace {
constexpr std::int64_t second = 1000000000;
constexpr std::int64_t day = 86400 * second;
constexpr std::size_t limit = 8 * 1024 * 1024;
// Display-only state, bounded to sixteen immutable dataset revisions and one
// checkpoint per source day (at most 7321 days each). Never persisted into user
// data. A hit still verifies all source segment digests that built its prefix;
// it saves Parquet decoding and EMA work without trusting file timestamps.
class MinuteIndicatorCache {
  using Key = std::pair<std::filesystem::path, std::string>;
  struct Entry {
    Key key;
    std::map<unsigned, chart_indicators::Macd> checkpoints;
  };
  std::mutex mutex_;
  std::list<Entry> entries_;

public:
  std::pair<unsigned, chart_indicators::Macd> find(const Key& key, unsigned page) {
    std::lock_guard lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->key != key)
        continue;
      const auto next = it->checkpoints.upper_bound(page);
      if (next == it->checkpoints.begin())
        return {};
      const auto checkpoint = *std::prev(next);
      entries_.splice(entries_.end(), entries_, it);
      return checkpoint;
    }
    return {};
  }
  void remember(const Key& key, const std::map<unsigned, chart_indicators::Macd>& checkpoints) {
    std::lock_guard lock(mutex_);
    auto entry = std::find_if(entries_.begin(), entries_.end(),
                              [&](const auto& value) { return value.key == key; });
    if (entry == entries_.end()) {
      if (entries_.size() == 16)
        entries_.pop_front();
      entries_.push_back({key, {}});
      entry = std::prev(entries_.end());
    }
    entry->checkpoints.insert(checkpoints.begin(), checkpoints.end());
    entries_.splice(entries_.end(), entries_, entry);
  }
};
MinuteIndicatorCache indicator_cache;
void validate_range(const HistoricalBarRange& range) {
  range.instrument.validate();
  validate_history_source(range.source);
  if (range.interval_minutes < 1 || range.interval_minutes > 1440 || range.begin_ns < 0 ||
      range.end_ns < range.begin_ns || range.end_ns - range.begin_ns > 20LL * 366 * day ||
      range.begin_ns % second || range.end_ns % second)
    throw std::invalid_argument("invalid historical minute range");
}
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
  return {{"contract_id", request.instrument.key()},
          {"source", request.source},
          {"source_instrument", request.source_instrument},
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
      spec.at("interval_minutes") > 1440)
    throw std::invalid_argument("invalid minute dataset specification");
  HistoricalBarRange range{HistoryIdentity::parse(spec.at("contract_id").get<std::string>()),
                           spec.at("interval_minutes").get<unsigned>(),
                           integer(spec.at("begin_ns")),
                           integer(spec.at("end_ns")),
                           spec.at("source").get<std::string>(),
                           spec.at("source_instrument").get<std::string>()};
  validate_range(range);
  if (specification(range) != spec)
    throw std::invalid_argument("invalid minute dataset specification");
  return range;
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
// One Parquet segment holds up to this many consecutive day pages.
constexpr unsigned segment_pages = 31;
std::string segment_name(unsigned first_page) {
  return "minutes-" + std::to_string(first_page) + ".parquet";
}
void validate_page(const std::vector<HistoricalBar>& bars, const HistoricalBarRange& range) {
  if (bars.size() >= 8000)
    throw std::invalid_argument("minute dataset page identity mismatch");
  std::int64_t previous = -1;
  for (const auto& bar : bars) {
    if (!bar.trading_day.empty())
      (void)parse_trading_date(bar.trading_day);
    if (bar.timestamp_ns <= previous || bar.timestamp_ns < range.begin_ns ||
        bar.timestamp_ns > range.end_ns || bar.timestamp_ns % (60 * second) ||
        bar.volume.raw() % 100000000)
      throw std::invalid_argument("invalid minute dataset order, interval or volume");
    previous = bar.timestamp_ns;
  }
}
std::uint64_t page_rows(const Json& manifest, unsigned page) {
  const auto& rows = manifest.at("pages").at(page);
  if (!rows.is_number_unsigned() || rows.get<std::uint64_t>() >= 8000)
    throw std::invalid_argument("minute dataset row count mismatch");
  return rows.get<std::uint64_t>();
}
std::filesystem::path verified_segment(const std::filesystem::path& dir, const Json& manifest,
                                       unsigned index) {
  const auto& segment = manifest.at("segments").at(index);
  const auto first = segment.at("first_page").get<unsigned>();
  const auto path = dir / segment_name(first);
  if (segment.at("file") != segment_name(first) || std::filesystem::is_symlink(path) ||
      !std::filesystem::is_regular_file(path) ||
      sha256_file(path) != segment.at("sha256").get<std::string>())
    throw std::invalid_argument("minute dataset digest mismatch");
  return path;
}
// Verifies one segment's digest and splits its bars back into day pages.
std::vector<std::vector<HistoricalBar>> load_segment(const std::filesystem::path& dir,
                                                     const Json& manifest, unsigned index,
                                                     const HistoricalBarRange& range) {
  const auto& segment = manifest.at("segments").at(index);
  const auto first = segment.at("first_page").get<unsigned>();
  const auto count = segment.at("pages").get<unsigned>();
  const auto path = verified_segment(dir, manifest, index);
  const auto bars = parquet::read_minute_bars(path);
  if (bars.size() != segment.at("rows").get<std::uint64_t>())
    throw std::invalid_argument("minute dataset row count mismatch");
  std::vector<std::vector<HistoricalBar>> pages;
  std::size_t offset = 0;
  for (unsigned page = first; page < first + count; ++page) {
    const auto rows = page_rows(manifest, page);
    if (rows > bars.size() - offset)
      throw std::invalid_argument("minute dataset row count mismatch");
    std::vector<HistoricalBar> part(bars.begin() + static_cast<std::ptrdiff_t>(offset),
                                    bars.begin() + static_cast<std::ptrdiff_t>(offset + rows));
    validate_page(part, page_range(range, page));
    offset += rows;
    pages.push_back(std::move(part));
  }
  if (offset != bars.size())
    throw std::invalid_argument("minute dataset row count mismatch");
  return pages;
}
Json verify(const std::filesystem::path& dir,
            const std::function<void(const HistoricalBar&)>& consume = {}) {
  const auto manifest = read_json(dir / manifest_name);
  if (manifest.at("version") != 4 || manifest.at("source") != manifest.at("request").at("source") ||
      manifest.at("timezone") != "Asia/Shanghai" || !valid_semantics(manifest.at("semantics")) ||
      manifest.at("semantics").at("source") != manifest.at("source") ||
      !manifest.at("pages").is_array() || !manifest.at("segments").is_array())
    throw std::invalid_argument("unsupported minute dataset format");
  (void)acquired_at(manifest);
  const auto range = range_of(manifest.at("request"));
  const auto total = total_pages(range);
  if (manifest.at("pages").size() > total ||
      manifest.at("complete") != (manifest.at("pages").size() == total))
    throw std::invalid_argument("invalid minute dataset coverage");
  if (consume && !manifest.at("complete").get<bool>())
    throw std::invalid_argument("minute dataset is incomplete");
  std::uint64_t rows = 0;
  unsigned covered = 0;
  for (unsigned i = 0; i < manifest.at("segments").size(); ++i) {
    const auto& segment = manifest.at("segments").at(i);
    require_fields(segment, {"file", "first_page", "pages", "rows", "sha256"});
    if (segment.at("first_page") != covered || !segment.at("pages").is_number_unsigned() ||
        segment.at("pages") < 1 || segment.at("pages") > segment_pages)
      throw std::invalid_argument("invalid minute dataset coverage");
    for (const auto& page : load_segment(dir, manifest, i, range))
      for (const auto& bar : page) {
        ++rows;
        if (consume)
          consume(bar);
      }
    covered += segment.at("pages").get<unsigned>();
  }
  if (covered != manifest.at("pages").size() || manifest.at("rows") != rows)
    throw std::invalid_argument("minute dataset row count mismatch");
  return manifest;
}
} // namespace
HistoricalBarRange minute_range(const data::v1::MinuteDownload& input) {
  if (input.version() != 2 || input.requests_per_minute() < 1 || input.requests_per_minute() > 500)
    throw std::invalid_argument("invalid minute download definition");
  HistoricalBarRange range{HistoryIdentity::parse(input.contract_id()),
                           input.interval_minutes(),
                           input.begin_ns(),
                           input.end_ns(),
                           input.source(),
                           input.source_instrument()};
  validate_range(range);
  (void)total_pages(range);
  return range;
}
data::v1::MinuteDownloadResult minute_result(const std::filesystem::path& dir) {
  auto manifest = inspect_minutes(dir);
  if (!manifest.at("complete").get<bool>())
    throw std::invalid_argument("minute dataset is incomplete");
  data::v1::MinuteDownloadResult result;
  result.set_version(2);
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
                      const std::filesystem::path& directory, std::stop_token stop,
                      DownloadProgress progress) {
  const auto semantics = encode_semantics(provider.semantics());
  if (provider.semantics().source != range.source)
    throw std::invalid_argument("historical source mismatch");
  validate_range(range);
  const auto total = total_pages(range);
  directory_check(directory);
  FileLock lock(directory, "minutes.lock");
  Json manifest;
  if (std::filesystem::exists(directory / manifest_name)) {
    manifest = verify(directory);
    if (manifest.at("request") != specification(range) || manifest.at("semantics") != semantics)
      throw std::invalid_argument("minute dataset request differs; choose a new directory");
  } else {
    for (const auto& file : std::filesystem::directory_iterator(directory))
      if (file.path().filename() != "minutes.lock")
        throw std::invalid_argument("minute dataset directory is not empty");
    manifest = {{"version", 4},
                {"source", range.source},
                {"timezone", "Asia/Shanghai"},
                {"semantics", semantics},
                {"request", specification(range)},
                {"pages", Json::array()},
                {"segments", Json::array()},
                {"rows", 0},
                {"complete", false},
                {"acquired_at_ns", "0"},
                {"source_availability", "unknown"}};
    replace_file_durably(directory / manifest_name, manifest.dump());
  }
  std::uint64_t rows = manifest.at("rows").get<std::uint64_t>();
  const auto committed = static_cast<unsigned>(manifest.at("pages").size());
  if (progress)
    progress(committed, total, rows);
  // Fetched pages wait in memory until a segment is written; cancellation and
  // provider failures still publish what was fetched before stopping.
  std::vector<std::vector<HistoricalBar>> pending;
  const auto flush = [&] {
    if (pending.empty())
      return;
    const auto first = static_cast<unsigned>(manifest.at("pages").size());
    std::vector<HistoricalBar> bars;
    for (auto& page : pending) {
      manifest["pages"].push_back(page.size());
      bars.insert(bars.end(), page.begin(), page.end());
    }
    const auto path = directory / segment_name(first);
    parquet::write_minute_bars(path, bars);
    rows += bars.size();
    manifest["segments"].push_back({{"file", segment_name(first)},
                                    {"first_page", first},
                                    {"pages", pending.size()},
                                    {"rows", bars.size()},
                                    {"sha256", sha256_file(path)}});
    manifest["rows"] = rows;
    complete_version(manifest, manifest.at("pages").size() == total);
    replace_file_durably(directory / manifest_name, manifest.dump());
    pending.clear();
  };
  try {
    for (unsigned i = committed; i < total; ++i) {
      if (stop.stop_requested())
        throw std::runtime_error("Historical download cancelled");
      const auto request = page_range(range, i);
      auto bars = provider.read(request, stop);
      // A page read while cancellation arrived is discarded, not published.
      if (stop.stop_requested())
        throw std::runtime_error("Historical download cancelled");
      for (const auto& bar : bars)
        bar.validate();
      validate_page(bars, request);
      pending.push_back(std::move(bars));
      if (pending.size() == segment_pages || i + 1 == total)
        flush();
      if (progress) {
        std::uint64_t fetched = rows;
        for (const auto& page : pending)
          fetched += page.size();
        progress(i + 1, total, fetched);
      }
    }
  } catch (...) {
    flush();
    throw;
  }
  return manifest;
}
std::vector<std::string> minute_trading_days(const std::filesystem::path& dir,
                                             const std::string& expected_revision) {
  directory_check(dir);
  FileLock lock(dir, "minutes.lock", FileLock::Access::shared);
  if (sha256_file(dir / manifest_name) != expected_revision)
    throw std::invalid_argument("historical archive revision mismatch");
  const auto manifest = read_json(dir / manifest_name);
  if (manifest.at("version") != 4 || !manifest.at("complete").get<bool>())
    throw std::invalid_argument("minute dataset is incomplete");
  std::vector<std::filesystem::path> files;
  for (const auto& segment : manifest.at("segments")) {
    const auto path = dir / segment_name(segment.at("first_page").get<unsigned>());
    if (segment.at("file") != path.filename().string() || std::filesystem::is_symlink(path) ||
        !std::filesystem::is_regular_file(path) ||
        sha256_file(path) != segment.at("sha256").get<std::string>())
      throw std::invalid_argument("minute dataset digest mismatch");
    files.push_back(path);
  }
  return parquet::trading_days(files, true);
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
                                      const data::v1::MinutePageQuery& query,
                                      MinutePageWork* work) {
  if (work)
    *work = {};
  if ((query.task_id().empty() && query.dataset_id().empty()) || query.limit() < 1 ||
      query.limit() > 200 || query.begin_ns() < 0 || query.end_ns() < 0)
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
  if (result.version() != 2 || sha256_bytes(manifest.dump()) != result.manifest_sha256() ||
      manifest.at("version") != 4 || manifest.at("request") != specification(range) ||
      !manifest.at("complete").get<bool>() || manifest.at("pages").size() != total_pages(range) ||
      manifest.at("rows") != result.rows())
    throw std::invalid_argument("minute dataset result does not match request");
  // Page index -> segment, from the committed manifest.
  std::vector<unsigned> owner;
  for (unsigned s = 0; s < manifest.at("segments").size(); ++s) {
    const auto& segment = manifest.at("segments").at(s);
    if (segment.at("first_page") != owner.size() || segment.at("pages") < 1 ||
        segment.at("pages") > segment_pages)
      throw std::invalid_argument("invalid minute dataset coverage");
    owner.insert(owner.end(), segment.at("pages").get<unsigned>(), s);
  }
  if (owner.size() != total_pages(range))
    throw std::invalid_argument("invalid minute dataset coverage");
  std::map<unsigned, std::vector<std::vector<HistoricalBar>>> loaded;
  const auto load = [&](unsigned index) -> const std::vector<HistoricalBar>& {
    const auto segment = owner.at(index);
    if (!loaded.contains(segment))
      loaded.emplace(segment, load_segment(dir, manifest, segment, range));
    const auto first = manifest.at("segments").at(segment).at("first_page").get<unsigned>();
    return loaded.at(segment).at(index - first);
  };
  data::v1::MinutePage output;
  output.set_version(2);
  output.set_task_id(query.dataset_id().empty() ? query.task_id() : query.dataset_id());
  output.set_source(input.source());
  output.set_contract_id(input.contract_id());
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
    const auto size = page_rows(manifest, i);
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
  const auto cache_key = std::pair{std::filesystem::canonical(dir), result.manifest_sha256()};
  std::map<unsigned, chart_indicators::Macd> checkpoints;
  std::uint64_t skip = query.offset();
  unsigned start = 0;
  if (query.include_macd()) {
    auto remaining = skip;
    unsigned target = 0;
    while (target < counts.size() && remaining >= counts[target])
      remaining -= counts[target++];
    const auto cached = indicator_cache.find(cache_key, target);
    start = cached.first;
    macd = cached.second;
    // A changed or missing prefix invalidates the response even on a cache hit.
    if (start)
      for (unsigned segment = 0; segment <= owner.at(start - 1); ++segment)
        if (!loaded.contains(segment))
          static_cast<void>(verified_segment(dir, manifest, segment));
    for (unsigned i = 0; i < start; ++i)
      skip -= counts[i];
    if (work)
      work->checkpoint_page = start;
  }
  for (unsigned i = start;
       i < counts.size() && output.bars_size() < static_cast<int>(query.limit()); ++i) {
    if (page_range(range, i).begin_ns > end)
      break;
    if (!query.include_macd() && skip >= counts[i]) {
      skip -= counts[i];
      continue;
    }
    if (query.include_macd())
      checkpoints.emplace(i, macd);
    for (const auto& bar : load(i)) {
      if (bar.timestamp_ns > end || output.bars_size() == static_cast<int>(query.limit()))
        break;
      const auto indicator = query.include_macd() ? macd.push(bar.close) : std::nullopt;
      if (work && query.include_macd())
        ++work->macd_rows;
      if (bar.timestamp_ns < begin || bar.timestamp_ns > end)
        continue;
      if (skip) {
        --skip;
        continue;
      }
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
    // Prefix scans retain only one validated source segment.
    if (i + 1 < owner.size() && owner[i + 1] != owner[i])
      loaded.erase(owner[i]);
  }
  if (query.include_macd())
    indicator_cache.remember(cache_key, checkpoints);
  return output;
}
} // namespace asterion::history_files
