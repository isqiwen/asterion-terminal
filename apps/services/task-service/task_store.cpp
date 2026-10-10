#include "risk_module.hpp"
#include "history_minutes.hpp"
#include "history_daily.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <fstream>
#include "task_store.hpp"
#include <asterion/protocol/task_execution.hpp>
#include <asterion/protocol/data.hpp>
#include "task_payload.hpp"
#include "sqlite_database.hpp"
#include "task_events.hpp"
#include <algorithm>
#include <asterion/foundation/decimal.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/protocol/factor.hpp>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#include <google/protobuf/util/json_util.h>
namespace asterion::tasks {
namespace wire = task::v1;
namespace fs = std::filesystem;
namespace {
void safe(const fs::path& path) {
  if (fs::is_symlink(path))
    throw std::invalid_argument("task store rejects symbolic links");
}
void prepare(wire::Task& task) {
  if (task.has_daily()) {
    const auto range = history_files::daily_range(task.daily());
    task.set_kind(wire::DAILY_DOWNLOAD);
    task.set_source_name(task.daily().source() + " " + task.daily().contract_id());
    task.set_instrument(range.instrument.key());
    task.set_total(history_files::daily_work_units(task.daily()));
  } else if (task.has_minutes()) {
    const auto range = history_files::minute_range(task.minutes());
    task.set_kind(wire::MINUTE_DOWNLOAD);
    task.set_source_name(task.minutes().source() + " " + task.minutes().contract_id());
    task.set_instrument(range.instrument.key());
    task.set_total(static_cast<unsigned>((range.end_ns - range.begin_ns) / 86400000000000LL + 1));
  } else if (task.has_factor()) {
    protocol::validate_factor_input(task.factor());
    task.set_kind(wire::FACTOR);
    std::string instruments, sources;
    for (const auto& series : task.factor().series()) {
      const auto separator = instruments.empty() ? "" : " + ";
      if (series.has_bars()) {
        const auto& c = series.bars().contract();
        instruments += separator + c.venue() + "/" + c.symbol();
        sources += separator + series.bars().revision();
      } else {
        instruments += separator + series.daily().contract_id();
        sources += separator + series.daily().source_dataset_id();
      }
    }
    task.set_instrument(instruments);
    task.set_source_name(sources);
    task.set_total(static_cast<unsigned>(protocol::factor_work_units(task.factor())));
    task.clear_trading_day();
  } else if (task.has_input()) {
    (void)protocol::decode_backtest(task.input(), protocol::DatasetView::metadata);
    task.set_kind(wire::BACKTEST);
    // Contracts share trading days; the first dataset describes them.
    const auto& paper = task.input().paper();
    const auto& dataset = paper.contracts(0).dataset();
    std::string instruments, sources;
    unsigned total = 0;
    for (const auto& contract : paper.contracts()) {
      const auto& c = contract.dataset().contract();
      instruments += (instruments.empty() ? "" : " + ") + c.venue() + "/" + c.symbol();
      sources += (sources.empty() ? "" : " + ") + contract.dataset().revision();
      total += static_cast<unsigned>(contract.dataset().bars_size());
    }
    task.set_instrument(instruments);
    task.set_source_name(sources);
    task.set_trading_day(dataset.days(0).trading_day());
    if (dataset.days_size() > 1)
      task.set_trading_day(task.trading_day() + " / " + dataset.days().rbegin()->trading_day());
    task.set_total(total);
  } else
    throw std::invalid_argument("task requires an explicit input type");
  task.set_data_source(task.has_minutes() ? task.minutes().source()
                       : task.has_daily() ? task.daily().source()
                                          : std::string{});
  task.set_minute_interval_minutes(task.has_minutes() ? task.minutes().interval_minutes() : 0);
  task.set_state(wire::QUEUED);
}
const google::protobuf::Message& definition(const wire::Task& task) {
  if (task.kind() == wire::DAILY_DOWNLOAD && task.has_daily())
    return task.daily();
  if (task.kind() == wire::MINUTE_DOWNLOAD && task.has_minutes())
    return task.minutes();
  if (task.kind() == wire::FACTOR && task.has_factor())
    return task.factor();
  if (task.kind() == wire::BACKTEST && task.has_input())
    return task.input();
  throw std::invalid_argument("task kind and input disagree");
}
// Lists, dispatch and durable transitions need bounded metadata, never a copy
// of the immutable task dataset. Definitions are loaded from owned files on demand.
wire::Task summary(const wire::Task& task) {
  wire::Task result;
  result.set_id(task.id());
  result.set_kind(task.kind());
  result.set_state(task.state());
  result.set_attempt(task.attempt());
  result.set_completed(task.completed());
  result.set_total(task.total());
  result.set_error(task.error());
  result.set_result_digest(task.result_digest());
  result.set_trading_day(task.trading_day());
  result.set_instrument(task.instrument());
  result.set_source_name(task.source_name());
  result.set_data_source(task.has_minutes() ? task.minutes().source()
                         : task.has_daily() ? task.daily().source()
                                            : task.data_source());
  result.set_history_dataset_id(task.history_dataset_id());
  if (task.has_publication())
    *result.mutable_publication() = task.publication();
  result.set_risk_artifact(task.risk_artifact());
  result.set_provider_artifact(task.provider_artifact());
  result.set_download_authorization(task.download_authorization());
  result.set_minute_interval_minutes(task.has_minutes() ? task.minutes().interval_minutes()
                                                        : task.minute_interval_minutes());
  result.set_submission_sequence(task.submission_sequence());
  result.set_submitted_at_ms(task.submitted_at_ms());
  result.set_updated_at_ms(task.updated_at_ms());
  return result;
}
void transition(const wire::Task& before, const std::string& old_token, const wire::Task& after,
                const std::string& token) {
  const auto a = before.state(), b = after.state();
  const bool start = a == wire::QUEUED && b == wire::RUNNING;
  const bool retry =
      (a == wire::FAILED || a == wire::INTERRUPTED || a == wire::CANCELLED) && b == wire::QUEUED;
  const bool running = a == wire::RUNNING &&
                       (b == wire::RUNNING || b == wire::CANCEL_REQUESTED || b == wire::SUCCEEDED ||
                        b == wire::PUBLISHING || b == wire::FAILED || b == wire::INTERRUPTED);
  const bool cancelling =
      a == wire::CANCEL_REQUESTED &&
      (b == wire::CANCEL_REQUESTED || b == wire::CANCELLED || b == wire::INTERRUPTED);
  if (!(start || retry || running || cancelling ||
        (a == wire::PUBLISHING && b == wire::SUCCEEDED) ||
        (a == wire::QUEUED && b == wire::CANCELLED)))
    throw std::invalid_argument("invalid durable task transition");
  if (after.attempt() > 100 || after.attempt() != before.attempt() + (start ? 1U : 0U) ||
      after.completed() > after.total())
    throw std::invalid_argument("invalid task attempt or progress");
  if (start || retry) {
    if (after.completed() != 0)
      throw std::invalid_argument("new attempt has progress");
  } else if (after.completed() < before.completed())
    throw std::invalid_argument("task progress moved backwards");
  if (start) {
    if (token.size() != 32 || token.find_first_not_of("0123456789abcdef") != std::string::npos ||
        token == old_token)
      throw std::invalid_argument("invalid new attempt token");
  } else if (retry || b == wire::INTERRUPTED || b == wire::PUBLISHING) {
    if (!token.empty())
      throw std::invalid_argument("inactive attempt retains token");
  } else if (token != old_token)
    throw std::invalid_argument("task token changed mid-attempt");
  const bool download =
      after.kind() == wire::MINUTE_DOWNLOAD || after.kind() == wire::DAILY_DOWNLOAD;
  if (b == wire::PUBLISHING || (download && b == wire::SUCCEEDED)) {
    const auto& publication = after.publication();
    const auto& identity = publication.identity();
    validate_id(identity.data_instance());
    validate_id(identity.task_instance());
    validate_id(publication.publication_id());
    if (!download || identity.data_instance() == identity.task_instance() ||
        identity.task_id() != after.id() || identity.attempt() != after.attempt() ||
        publication.candidate_digest().size() != 64 ||
        publication.candidate_digest().find_first_not_of("0123456789abcdef") != std::string::npos ||
        after.history_dataset_id().size() != 64 ||
        after.history_dataset_id().find_first_not_of("0123456789abcdef") != std::string::npos ||
        (b == wire::SUCCEEDED && a != wire::PUBLISHING))
      throw std::invalid_argument("invalid task publication decision");
  } else if (after.has_publication() || !after.history_dataset_id().empty())
    throw std::invalid_argument("task retains an unexpected publication decision");
  if (a == wire::PUBLISHING &&
      (before.publication().SerializeAsString() != after.publication().SerializeAsString() ||
       before.history_dataset_id() != after.history_dataset_id() ||
       before.result_digest() != after.result_digest()))
    throw std::invalid_argument("task publication identity changed");
  if (b == wire::SUCCEEDED || b == wire::PUBLISHING) {
    if (after.completed() != after.total() || after.result_digest().size() != 64 ||
        after.result_digest().find_first_not_of("0123456789abcdef") != std::string::npos)
      throw std::invalid_argument("invalid committed result identity");
  } else if (!after.result_digest().empty())
    throw std::invalid_argument("unfinished task has result digest");
  if (after.error().size() > 1024)
    throw std::invalid_argument("task error exceeds limit");
}
void verify_result(const wire::Task& task, const asterion::backtest::v1::BacktestResult& result) {
  protocol::validate_message(result);
  if (task.kind() != wire::BACKTEST || !task.has_input())
    throw std::invalid_argument("not a backtest task");
  const auto& paper = task.input().paper();
  // Months of a dominant series trade on part of the days only: the replay
  // settles every day any contract trades, and costs follow its last day.
  std::set<std::string> trading_days;
  for (const auto& contract : paper.contracts())
    for (const auto& day : contract.dataset().days())
      trading_days.insert(day.trading_day());
  const auto days = static_cast<int>(trading_days.size());
  const auto last_day = trading_days.empty() ? std::string() : *trading_days.rbegin();
  bool contracts = result.account().contracts_size() == paper.contracts_size();
  for (int c = 0; contracts && c < paper.contracts_size(); ++c)
    contracts =
        result.account().contracts(c).contract().SerializeAsString() ==
            paper.contracts(c).dataset().contract().SerializeAsString() &&
        result.account().contracts(c).costs().SerializeAsString() ==
            protocol::encode_costs(
                costs_on(protocol::cost_schedule(paper.contracts(c).cost_schedule()), last_day)
                    .values)
                .SerializeAsString() &&
        result.account().contracts(c).cost_schedule().SerializeAsString() ==
            paper.contracts(c).cost_schedule().SerializeAsString();
  if (result.version() != 5 || result.dataset_revision() != task.input().dataset_revision() ||
      result.engine_version() != protocol::backtest_engine_version ||
      result.account().cursor() != task.total() || result.account().total() != task.total() ||
      result.equity_size() != static_cast<int>(task.total()) + days ||
      result.settlements_size() != days || !contracts ||
      result.account().risk().SerializeAsString() != paper.risk().SerializeAsString() ||
      !result.has_max_drawdown() || result.account().recovery_required())
    throw std::invalid_argument("incomplete or mismatched task result");
  static_cast<void>(protocol::decode_backtest_result(result));
  auto peak = Decimal::from_raw(task.input().paper().deposit().units());
  Decimal drawdown;
  for (int i = 0; i < result.equity_size(); ++i) {
    const auto& point = result.equity(i);
    if (!point.has_equity() ||
        (i > 0 && point.timestamp_ns() < result.equity(i - 1).timestamp_ns()))
      throw std::invalid_argument("result curve does not match input timeline");
    const auto equity = Decimal::from_raw(point.equity().units());
    peak = std::max(peak, equity);
    drawdown = std::max(drawdown, peak - equity);
  }
  if (result.equity().rbegin()->equity().units() != result.account().equity().units() ||
      drawdown.raw() != result.max_drawdown().units() || result.account().frozen().units() != 0)
    throw std::invalid_argument("inconsistent result summary");
  for (const auto& order : result.account().orders())
    if (order.state() == protocol::v1::ACCEPTED || order.state() == protocol::v1::PARTIALLY_FILLED)
      throw std::invalid_argument("result retains active orders");
  std::vector<const protocol::v1::Bar*> timeline;
  for (const auto& contract : paper.contracts())
    for (const auto& bar : contract.dataset().bars())
      timeline.push_back(&bar);
  std::ranges::stable_sort(timeline, {}, [](const auto* bar) { return bar->timestamp_ns(); });
  int point_index = 0, settlement_index = 0;
  for (std::size_t i = 0; i < timeline.size(); ++i) {
    const auto check_point = [&](asterion::backtest::v1::EquityEvent event)
        -> const asterion::backtest::v1::EquityPoint& {
      if (point_index >= result.equity_size())
        throw std::invalid_argument("result curve does not match input timeline");
      const auto& point = result.equity(point_index++);
      if (point.timestamp_ns() != timeline[i]->timestamp_ns() || point.event() != event)
        throw std::invalid_argument("result curve does not match input timeline");
      return point;
    };
    check_point(asterion::backtest::v1::TRADE_MARK);
    if (i + 1 == timeline.size() || timeline[i]->trading_day() != timeline[i + 1]->trading_day()) {
      const auto& point = check_point(asterion::backtest::v1::DAILY_SETTLEMENT);
      if (settlement_index >= result.settlements_size())
        throw std::invalid_argument("settlement result does not match input days");
      const auto& day = result.settlements(settlement_index++);
      if (day.timestamp_ns() != point.timestamp_ns() ||
          day.equity().units() != point.equity().units())
        throw std::invalid_argument("inconsistent result summary");
    }
  }
  if (point_index != result.equity_size() || settlement_index != result.settlements_size())
    throw std::invalid_argument("result curve does not match input timeline");
  // Settlement evidence must name the input's exact dates, contracts and prices.
  int day_index = 0;
  for (const auto& day : trading_days) {
    const auto& settlement = result.settlements(day_index++);
    if (settlement.trading_day() != day)
      throw std::invalid_argument("settlement result does not match input days");
    int contract_index = 0;
    for (const auto& contract : paper.contracts()) {
      const auto found = std::ranges::find_if(
          contract.dataset().days(), [&](const auto& value) { return value.trading_day() == day; });
      if (found == contract.dataset().days().end())
        continue;
      if (contract_index == settlement.contracts_size())
        throw std::invalid_argument("settlement result does not match input contracts");
      const auto& value = settlement.contracts(contract_index++);
      if (value.venue() != contract.dataset().contract().venue() ||
          value.symbol() != contract.dataset().contract().symbol() ||
          value.price().units() != found->settlement_price().units())
        throw std::invalid_argument("settlement result does not match input contracts");
    }
    if (contract_index != settlement.contracts_size())
      throw std::invalid_argument("settlement result does not match input contracts");
  }
}
wire::Task read_input(wire::Task task, const fs::path& path, const std::string& digest,
                      std::uint64_t bytes) {
  const auto raw = payload::read(path, digest, payload::max_input_bytes, bytes);
  switch (task.kind()) {
  case wire::DAILY_DOWNLOAD:
    payload::parse(raw, *task.mutable_daily());
    break;
  case wire::MINUTE_DOWNLOAD:
    payload::parse(raw, *task.mutable_minutes());
    break;
  case wire::FACTOR:
    payload::parse(raw, *task.mutable_factor());
    break;
  case wire::BACKTEST:
    payload::parse(raw, *task.mutable_input());
    break;
  default:
    throw std::invalid_argument("unsupported task manifest type");
  }
  return task;
}
wire::Task attach_definition(wire::Task task, wire::Task source) {
  switch (task.kind()) {
  case wire::DAILY_DOWNLOAD:
    *task.mutable_daily() = std::move(*source.mutable_daily());
    break;
  case wire::MINUTE_DOWNLOAD:
    *task.mutable_minutes() = std::move(*source.mutable_minutes());
    break;
  case wire::FACTOR:
    *task.mutable_factor() = std::move(*source.mutable_factor());
    break;
  case wire::BACKTEST:
    *task.mutable_input() = std::move(*source.mutable_input());
    break;
  default:
    throw std::invalid_argument("unsupported task manifest type");
  }
  return task;
}
bool active(wire::TaskState state) {
  return state == wire::RUNNING || state == wire::CANCEL_REQUESTED;
}
bool occupies_slot(wire::TaskState state) {
  return state == wire::QUEUED || active(state) || state == wire::PUBLISHING;
}
} // namespace
struct Store::Impl {
  using ValidatedInputs = std::map<std::pair<wire::TaskKind, std::string>, wire::Task>;
  struct Entry {
    wire::Task task; // Metadata only; immutable input stays on disk.
    std::string input_digest;
    std::uint64_t input_bytes = 0;
    std::string token;
    std::uint64_t event_sequence = 0;
  };
  using ActiveEntries = std::map<std::string, Entry>;
  // The state owner retains only admitted work. SQLite is its durable projection
  // and the on-demand index for terminal history, not a second active-state owner.
  ActiveEntries current;
  struct PendingChange {
    Entry next;
    std::string record, event, manifest, kind;
    ActiveEntries::node_type active_entry;
  };
  fs::path root;
  Identity identity;
  std::unique_ptr<FileLock> owner;
  // Task index and state history: one row per task, one per state change.
  std::unique_ptr<sqlite::Database> database;
  std::unique_ptr<sqlite::TaskEvents> event_log;

  // A submission interrupted after creating its directory but before its index
  // commit leaves the directory; it is not loaded and its ID stays reserved.
  std::uint32_t uncommitted = 0;
  std::optional<std::string> submitting;
  bool failed = false;
  bool read_only = false;
  std::shared_ptr<const Clock> clock;
  std::uint32_t last_sequence = 0;
  std::int64_t now_ms() const {
    const auto value = clock->utc_now() / 1000000;
    if (value <= 0)
      throw std::invalid_argument("task clock must report a positive UTC time");
    return value;
  }
  explicit Impl(fs::path directory, Identity binding, std::shared_ptr<const Clock> source,
                bool inspect, bool bounded_inspection = true,
                ValidatedInputs* shared_validation = nullptr)
      : root(std::move(directory)), identity(std::move(binding)), read_only(inspect),
        clock(std::move(source)) {
    if (!clock)
      throw std::invalid_argument("task store requires a clock");
    std::uint64_t inspection_bytes = 0, inspection_records = 0;
    const auto inspect_size = [&](std::int64_t bytes, std::int64_t maximum) {
      if (bytes <= 0 || bytes > maximum)
        throw std::invalid_argument("invalid task record size");
      if (read_only && bounded_inspection &&
          ((inspection_bytes += bytes) > 256 * 1024 * 1024 || ++inspection_records > 100000))
        throw std::invalid_argument("task reference inspection limit exceeded");
    };
    // Repeated experiments may share identical immutable definitions. Cache
    // only their validated projection for this restore; every owned file is
    // still checked independently. This cache is bounded independently of history size.
    ValidatedInputs own_validation;
    auto& validated_inputs = shared_validation ? *shared_validation : own_validation;
    if (!root.is_absolute() || !fs::is_directory(root))
      throw std::invalid_argument("task store requires an existing absolute directory");
    safe(root);
    if (fs::exists(root / "history"))
      throw std::invalid_argument("task store contains an unsupported historical warehouse");
    validate_id(identity.instance);
    validate_id(identity.data_instance);
    const Json expected{{"version", 3},
                        {"task_instance", identity.instance},
                        {"data_instance", identity.data_instance}};
    const auto identity_path = root / "instance.json";
    const auto check_identity = [&] {
      safe(identity_path);
      if (!fs::exists(identity_path))
        return false;
      if (!fs::is_regular_file(identity_path) || fs::file_size(identity_path) > 4096)
        throw std::invalid_argument("invalid task instance identity");
      std::ifstream stream(identity_path, std::ios::binary);
      const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
      if (stream.fail())
        throw std::runtime_error("task instance identity read failed");
      if (parse_json(bytes) != expected)
        throw std::invalid_argument("task service instance or data binding mismatch");
      return true;
    };
    const auto require_new_directory = [&] {
      if (read_only)
        throw std::invalid_argument("task directory has no supported instance identity");
      for (const auto& file : fs::directory_iterator(root))
        if (file.path().filename() != "manager.lock" || !file.is_regular_file() ||
            file.is_symlink())
          throw std::invalid_argument("task directory has no supported instance identity");
    };
    if (!check_identity())
      require_new_directory();
    const bool existing = fs::exists(root / "tasks.sqlite");
    const auto image = [&] {
      const auto wal = root / "tasks.sqlite-wal";
      safe(wal);
      return std::pair{sha256_file(root / "tasks.sqlite"),
                       fs::exists(wal) && fs::file_size(wal) ? sha256_file(wal) : std::string{}};
    };
    std::optional<std::pair<std::string, std::string>> validated_image;
    if (!read_only && existing) {
      // Validate every manifest and transition under a shared ownership lock,
      // with a read-only database. Unsupported stores
      // must be rejected before creating directories or writer journal state.
      // Reuse semantic projections across the read-only preflight and writer
      // validation, but recheck every file digest after taking exclusive ownership.
      Impl inspected(root, identity, clock, true, false, &validated_inputs);
      validated_image = image();
    }
    owner = std::make_unique<FileLock>(root, "manager.lock",
                                       read_only ? FileLock::Access::shared_existing
                                                 : FileLock::Access::exclusive);
    // Recheck after acquiring ownership: another process may have initialized
    // this new directory before we acquired its lock.
    if (!check_identity()) {
      require_new_directory();
      replace_file_durably(identity_path, expected.dump());
    } else if (!read_only) {
      sync_directory(root);
    }
    if (validated_image && image() != *validated_image)
      throw std::invalid_argument("task store changed after read-only validation");
    safe(root / "tasks.sqlite");
    safe(root / "tasks.sqlite-wal");
    safe(root / "tasks.sqlite-shm");
    database = std::make_unique<sqlite::Database>(
        root / "tasks.sqlite",
        read_only ? sqlite::Database::Access::read_only : sqlite::Database::Access::writer,
        [&](sqlite::Database&) {
          if (validated_image && image() != *validated_image)
            throw std::invalid_argument("task store changed after read-only validation");
        });
    if (read_only)
      database->execute("BEGIN");
    else {
      sqlite::Database::Transaction schema(*database);
      database->execute("CREATE TABLE IF NOT EXISTS tasks(id TEXT PRIMARY KEY,"
                        " sequence INTEGER NOT NULL UNIQUE, kind TEXT NOT NULL,"
                        " manifest TEXT NOT NULL, state INTEGER NOT NULL,"
                        " attempt INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL, record BLOB "
                        "NOT NULL) STRICT");
      database->execute("CREATE INDEX IF NOT EXISTS tasks_state ON tasks(state, sequence)");
      database->execute("CREATE TABLE IF NOT EXISTS task_events(task_id TEXT NOT NULL"
                        " REFERENCES tasks(id), sequence INTEGER NOT NULL, body TEXT NOT NULL,"
                        " PRIMARY KEY(task_id, sequence)) STRICT");
      schema.commit();
    }
    event_log = std::make_unique<sqlite::TaskEvents>(*database, root / "event-segments", read_only);
    for (const auto& item : fs::directory_iterator(root)) {
      safe(item.path());
      const auto name = item.path().filename().string();
      if (name == "event-segments" && item.is_directory())
        continue;
      if (item.is_regular_file() && (name == "instance.json" || name == "manager.lock" ||
                                     name == "tasks.sqlite" || name == "tasks.sqlite-wal" ||
                                     name == "tasks.sqlite-shm" || name == "tasks.sqlite-journal"))
        continue;
      validate_id(name);
      if (!item.is_directory())
        throw std::invalid_argument("unknown task store entry");
      if (!contains(name))
        ++uncommitted;
    }
    event_log->read([&](const std::string& id, std::uint64_t, const std::string&) {
      if (!contains(id))
        throw std::invalid_argument("invalid persisted task event identity");
    });
    sqlite::Database::Statement tasks(
        *database, "SELECT id, CASE WHEN length(CAST(manifest AS BLOB)) <= 65536 THEN manifest "
                   "END, length(CAST(manifest AS BLOB)), sequence FROM tasks ORDER BY sequence");
    while (tasks.step()) {
      const auto id = tasks.text(0);
      validate_id(id);
      if (!fs::is_directory(root / id))
        throw std::invalid_argument("indexed task directory is missing");
      safe(root / id / "results");
      inspect_size(tasks.integer(2), 65536);
      const auto manifest = parse_json(tasks.text(1), 65536);
      Entry entry;
      if (!manifest.is_object() || manifest.value("version", 0) != 7)
        throw std::invalid_argument("unsupported task manifest");
      require_fields(manifest, {"version", "type", "input_sha256", "input_bytes", "id",
                                "submission_sequence", "submitted_at_ms", "provider_artifact",
                                "risk_artifact", "download_authorization"});
      if (manifest.at("id") != id)
        throw std::invalid_argument("unsupported task manifest");
      if (!manifest.at("submission_sequence").is_number_unsigned() ||
          manifest.at("submission_sequence") < 1 ||
          manifest.at("submission_sequence") > std::numeric_limits<std::uint32_t>::max() ||
          !manifest.at("submitted_at_ms").is_number_integer() ||
          manifest.at("submitted_at_ms") <= 0 || manifest.at("submitted_at_ms") > 9223372036854LL)
        throw std::invalid_argument("invalid task submission metadata");
      if (!manifest.at("input_bytes").is_number_unsigned() || manifest.at("input_bytes") < 1 ||
          manifest.at("input_bytes") > payload::max_input_bytes)
        throw std::invalid_argument("invalid task input size");
      entry.input_bytes = manifest.at("input_bytes").get<std::uint64_t>();
      entry.input_digest = manifest.at("input_sha256").get<std::string>();
      payload::validate_digest(entry.input_digest);
      inspect_size(static_cast<std::int64_t>(entry.input_bytes), payload::max_input_bytes);
      entry.task.set_submission_sequence(manifest.at("submission_sequence").get<std::uint32_t>());
      entry.task.set_submitted_at_ms(manifest.at("submitted_at_ms").get<std::int64_t>());
      entry.task.set_updated_at_ms(entry.task.submitted_at_ms());
      if (entry.task.submission_sequence() != ++last_sequence || tasks.integer(3) != last_sequence)
        throw std::invalid_argument("task submission sequence has missing records");
      entry.task.set_id(id);
      if (manifest.at("type") == "daily.task")
        entry.task.set_kind(wire::DAILY_DOWNLOAD);
      else if (manifest.at("type") == "minutes.task")
        entry.task.set_kind(wire::MINUTE_DOWNLOAD);
      else if (manifest.at("type") == "backtest.task")
        entry.task.set_kind(wire::BACKTEST);
      else if (manifest.at("type") == "factor.task")
        entry.task.set_kind(wire::FACTOR);
      else
        throw std::invalid_argument("unsupported task manifest type");
      entry.task.set_provider_artifact(manifest.at("provider_artifact").get<std::string>());
      entry.task.set_download_authorization(
          manifest.at("download_authorization").get<std::string>());
      if (entry.task.kind() == wire::MINUTE_DOWNLOAD || entry.task.kind() == wire::DAILY_DOWNLOAD) {
        if (entry.task.download_authorization().size() != 64 ||
            entry.task.download_authorization().find_first_not_of("0123456789abcdef") !=
                std::string::npos)
          throw std::invalid_argument("invalid task download authorization");
        const auto& hash = entry.task.provider_artifact();
        if (hash.size() != 64 || !std::ranges::all_of(hash, [](char c) {
              return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))
          throw std::invalid_argument("invalid task provider artifact");
      } else if (!entry.task.provider_artifact().empty() ||
                 !entry.task.download_authorization().empty())
        throw std::invalid_argument("unexpected task provider artifact");
      entry.task.set_risk_artifact(manifest.at("risk_artifact").get<std::string>());
      if (entry.task.kind() == wire::BACKTEST) {
        const auto& hash = entry.task.risk_artifact();
        if (hash.size() != 64 || !std::ranges::all_of(hash, [](char c) {
              return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))
          throw std::invalid_argument("invalid risk plugin artifact");
      } else if (!entry.task.risk_artifact().empty())
        throw std::invalid_argument("unexpected task risk artifact");
      const auto input_identity = std::pair{entry.task.kind(), entry.input_digest};
      auto validated = validated_inputs.find(input_identity);
      if (validated == validated_inputs.end()) {
        // Validate one definition, retain its projection, then release the
        // large payload before advancing to the next task.
        auto task = load(entry);
        prepare(task);
        if (validated_inputs.size() == 64)
          validated_inputs.clear();
        validated = validated_inputs.emplace(input_identity, summary(task)).first;
      } else
        payload::check(root / id / "input.pb", entry.input_digest, payload::max_input_bytes,
                       entry.input_bytes);
      const auto& projection = validated->second;
      entry.task.set_total(projection.total());
      entry.task.set_trading_day(projection.trading_day());
      entry.task.set_instrument(projection.instrument());
      entry.task.set_source_name(projection.source_name());
      entry.task.set_data_source(projection.data_source());
      entry.task.set_minute_interval_minutes(projection.minute_interval_minutes());
      entry.task.set_state(wire::QUEUED);
      event_log->read_task(
          id, [&](const std::string&, std::uint64_t sequence, const std::string& raw) {
            if (sequence != ++entry.event_sequence)
              throw std::invalid_argument("task state history has a gap");
            inspect_size(static_cast<std::int64_t>(raw.size()), 65536);
            const auto event = parse_json(raw, 65536);
            require_fields(event, {"version", "state", "attempt", "token", "completed", "error",
                                   "digest", "updated_at_ms", "publication", "history_dataset_id"});
            if (event.at("version") != 3)
              throw std::invalid_argument("unsupported task state version");
            const auto state = event.at("state").get<int>();
            if (!wire::TaskState_IsValid(state) || state == 0 ||
                !event.at("attempt").is_number_unsigned() ||
                !event.at("completed").is_number_unsigned())
              throw std::invalid_argument("invalid persisted task state");
            const auto completed = event.at("completed").get<unsigned>();
            if (completed > entry.task.total())
              throw std::invalid_argument("invalid persisted task progress");
            if (!event.at("updated_at_ms").is_number_integer() || event.at("updated_at_ms") <= 0 ||
                event.at("updated_at_ms") > 9223372036854LL)
              throw std::invalid_argument("invalid task update time");
            const auto previous = summary(entry.task);
            entry.task.set_updated_at_ms(event.at("updated_at_ms").get<std::int64_t>());
            const auto old_token = entry.token;
            entry.task.set_state(static_cast<wire::TaskState>(state));
            entry.task.set_attempt(event.at("attempt").get<unsigned>());
            entry.task.set_completed(completed);
            entry.task.set_error(event.at("error").get<std::string>());
            entry.task.set_result_digest(event.at("digest").get<std::string>());
            entry.task.set_history_dataset_id(event.at("history_dataset_id").get<std::string>());
            entry.task.clear_publication();
            if (!event.at("publication").is_null() &&
                !google::protobuf::util::JsonStringToMessage(event.at("publication").dump(),
                                                             entry.task.mutable_publication())
                     .ok())
              throw std::invalid_argument("invalid persisted publication decision");
            entry.token = event.at("token").get<std::string>();
            transition(previous, old_token, entry.task, entry.token);
          });
      if (encode(entry) != encode(find(id)))
        throw std::invalid_argument("task index does not match state history");
      if (entry.task.has_publication() &&
          (entry.task.publication().identity().task_instance() != identity.instance ||
           entry.task.publication().identity().data_instance() != identity.data_instance))
        throw std::invalid_argument("task publication belongs to another service binding");
      if (entry.task.state() == wire::SUCCEEDED || entry.task.state() == wire::PUBLISHING)
        check_result_digest(entry);
      if (occupies_slot(entry.task.state())) {
        if (current.size() == 1000)
          throw std::invalid_argument("task active capacity exceeded");
        current.emplace(id, std::move(entry));
      }
    }
    // Recovery retains bounded active metadata, one input and one event segment.
    // Terminal history is validated and then released, never copied into current.
    if (read_only)
      return;
    for (const auto& task : active_tasks()) {
      if (active(task.state())) {
        auto entry = find(task.id());
        auto next = summary(entry.task);
        next.set_state(wire::INTERRUPTED);
        next.set_error("task service restarted before confirmed completion; "
                       "explicit retry required");
        commit(entry, next, "");
      }
    }
  }
  static std::string encode(const Entry& entry) {
    wire::StoredTaskRecord record;
    *record.mutable_task() = entry.task;
    record.set_input_digest(entry.input_digest);
    record.set_input_bytes(entry.input_bytes);
    record.set_token(entry.token);
    record.set_event_sequence(entry.event_sequence);
    if (record.ByteSizeLong() > 65536)
      throw std::invalid_argument("task metadata exceeds limit");
    return record.SerializeAsString();
  }
  bool contains(const std::string& id) const {
    sqlite::Database::Statement query(*database, "SELECT 1 FROM tasks WHERE id=?");
    query.bind(1, id);
    return query.step();
  }
  std::uint32_t active_count() const { return static_cast<std::uint32_t>(current.size()); }
  void admit() const {
    if (active_count() + (submitting ? 1 : 0) >= 1000)
      throw std::invalid_argument("task active capacity reached");
  }
  std::vector<wire::Task> active_tasks() const {
    std::vector<wire::Task> result;
    result.reserve(current.size());
    for (const auto& [id, entry] : current)
      result.push_back(entry.task);
    std::ranges::sort(result, {}, &wire::Task::submission_sequence);
    return result;
  }
  wire::Task load(const Entry& entry) const {
    return read_input(entry.task, root / entry.task.id() / "input.pb", entry.input_digest,
                      entry.input_bytes);
  }
  void writable() const {
    if (read_only)
      throw std::logic_error("task store is read only");
    if (failed)
      throw std::runtime_error("task store requires recovery");
  }
  Entry find(const std::string& id) const {
    validate_id(id);
    if (const auto found = current.find(id); found != current.end())
      return found->second;
    sqlite::Database::Statement query(
        *database, "SELECT CASE WHEN length(record)<=65536 THEN record END, "
                   "state, attempt, updated_at_ms, sequence FROM tasks WHERE id=?");
    query.bind(1, id);
    if (!query.step())
      throw std::out_of_range("task does not exist");
    wire::StoredTaskRecord stored;
    const auto raw = query.blob(0);
    if (raw.empty() || !stored.ParseFromString(raw))
      throw std::invalid_argument("invalid task metadata record");
    protocol::validate_message(stored);
    if (stored.task().id() != id || stored.task().state() != query.integer(1) ||
        stored.task().attempt() != query.integer(2) ||
        stored.task().updated_at_ms() != query.integer(3) ||
        stored.task().submission_sequence() != query.integer(4))
      throw std::invalid_argument("task index does not match state history");
    return {stored.task(), stored.input_digest(), stored.input_bytes(), stored.token(),
            stored.event_sequence()};
  }
  static ActiveEntries::node_type active_entry(const Entry& entry) {
    if (!occupies_slot(entry.task.state()))
      return {};
    ActiveEntries prepared;
    prepared.emplace(entry.task.id(), entry);
    return prepared.extract(prepared.begin());
  }
  PendingChange prepare_change(const Entry& entry, wire::Task next, const std::string& token) {
    writable();
    next.set_updated_at_ms(now_ms());
    transition(entry.task, entry.token, next, token);
    PendingChange change;
    change.next = {std::move(next), entry.input_digest, entry.input_bytes, token,
                   entry.event_sequence + 1};
    change.record = encode(change.next);
    change.active_entry = active_entry(change.next);
    const auto& task = change.next.task;
    Json publication = nullptr;
    if (task.has_publication()) {
      std::string json;
      if (!google::protobuf::util::MessageToJsonString(task.publication(), &json).ok())
        throw std::logic_error("cannot encode task publication decision");
      publication = parse_json(json);
    }
    change.event = Json{{"version", 3},
                        {"publication", publication},
                        {"history_dataset_id", task.history_dataset_id()},
                        {"updated_at_ms", task.updated_at_ms()},
                        {"state", static_cast<int>(task.state())},
                        {"attempt", task.attempt()},
                        {"token", token},
                        {"completed", task.completed()},
                        {"error", task.error()},
                        {"digest", task.result_digest()}}
                       .dump();
    return change;
  }
  // Persistence consumes an already decided and encoded transition. It neither
  // reads current state nor chooses cancellation, attempt or completion semantics.
  void persist(const PendingChange& change) {
    const auto& next = change.next.task;
    if (!change.manifest.empty()) {
      sqlite::Database::Transaction transaction(*database);
      sqlite::Database::Statement insert(*database,
                                         "INSERT INTO tasks VALUES(?, ?, ?, ?, ?, 0, ?, ?)");
      insert.bind(1, next.id())
          .bind(2, static_cast<std::int64_t>(next.submission_sequence()))
          .bind(3, change.kind)
          .bind(4, change.manifest)
          .bind(5, static_cast<std::int64_t>(next.state()))
          .bind(6, next.submitted_at_ms())
          .bind_blob(7, change.record)
          .step();
      transaction.commit();
      return;
    }
    event_log->rotate();
    sqlite::Database::Transaction transaction(*database);
    event_log->append(next.id(), change.next.event_sequence, change.event);
    sqlite::Database::Statement update(
        *database, "UPDATE tasks SET state=?, attempt=?, updated_at_ms=?, record=? WHERE id=?");
    update.bind(1, static_cast<std::int64_t>(next.state()))
        .bind(2, static_cast<std::int64_t>(next.attempt()))
        .bind(3, next.updated_at_ms())
        .bind_blob(4, change.record)
        .bind(5, next.id())
        .step();
    transaction.commit();
  }
  void publish_active(const std::string& id, ActiveEntries::node_type prepared) {
    current.erase(id);
    if (prepared)
      current.insert(std::move(prepared));
  }
  void commit(Entry& entry, wire::Task next, const std::string& token) {
    auto change = prepare_change(entry, std::move(next), token);
    try {
      persist(change);
    } catch (...) {
      failed = true;
      throw;
    }
    // The map node and result are allocated before persistence. Confirmation
    // only transfers ownership and cannot allocate after the durable barrier.
    publish_active(change.next.task.id(), std::move(change.active_entry));
    entry = std::move(change.next);
  }
  Change change(const Entry&, wire::Task, const std::string&);
  Change unchanged(Entry);
  Entry fenced(const std::string& id, const std::string& token) {
    writable();
    auto entry = find(id);
    if (token.empty() || token != entry.token || !active(entry.task.state()))
      throw std::invalid_argument("stale or inactive task attempt");
    return entry;
  }
  fs::path result_path(const Entry& entry) const {
    return root / entry.task.id() / "results" / (std::to_string(entry.task.attempt()) + ".pb");
  }
  void check_result_digest(const Entry& entry) const {
    payload::check(result_path(entry), entry.task.result_digest(), payload::max_result_bytes);
  }
  ResultRead prepare_read(const Entry& entry, bool deep, bool include_input) const {
    return ResultRead(entry.task, root / entry.task.id(), entry.input_digest, entry.input_bytes,
                      deep, include_input);
  }
  wire::TaskResponse read_result(const Entry& entry, bool deep = true) const {
    auto read = prepare_read(entry, deep, false);
    read.verify();
    return std::move(read.response_);
  }
};
struct Store::Change::State {
  Impl* owner;
  std::optional<Impl::PendingChange> pending;
  Impl::Entry unchanged;
  bool written = false, confirmed = false;
  const Impl::Entry& result() const { return pending ? pending->next : unchanged; }
};
Store::Change::Change(std::unique_ptr<State> state) : state_(std::move(state)) {}
Store::Change::Change(Change&&) noexcept = default;
Store::Change& Store::Change::operator=(Change&&) noexcept = default;
Store::Change::~Change() = default;
bool Store::Change::needs_write() const {
  return state_->pending.has_value();
}
const wire::Task& Store::Change::task() const {
  return state_->result().task;
}
const std::string& Store::Change::token() const {
  return state_->result().token;
}
void Store::Change::persist() {
  if (state_->written)
    throw std::logic_error("task change is already persisted");
  if (state_->pending)
    state_->owner->persist(*state_->pending);
  state_->written = true;
}
Store::Change Store::Impl::change(const Entry& entry, wire::Task next, const std::string& token) {
  auto state = std::make_unique<Change::State>();
  state->owner = this;
  state->pending.emplace(prepare_change(entry, std::move(next), token));
  return Change(std::move(state));
}
Store::Change Store::Impl::unchanged(Entry entry) {
  auto state = std::make_unique<Change::State>();
  state->owner = this;
  state->unchanged = std::move(entry);
  return Change(std::move(state));
}
void Store::confirm(Change& change) {
  auto& state = *change.state_;
  if (state.owner != impl_.get() || !state.written || state.confirmed)
    throw std::logic_error("task change is not awaiting confirmation");
  if (state.pending) {
    impl_->publish_active(state.pending->next.task.id(), std::move(state.pending->active_entry));
    if (!state.pending->manifest.empty()) {
      impl_->last_sequence = state.pending->next.task.submission_sequence();
      impl_->submitting.reset();
    }
  }
  state.confirmed = true;
}
void Store::persistence_failed() {
  impl_->failed = true;
}
bool Store::is_active(const std::string& id) const {
  return impl_->current.contains(id);
}
Store::Change Store::commit(Change change) {
  try {
    change.persist();
  } catch (...) {
    persistence_failed();
    throw;
  }
  confirm(change);
  return change;
}
Store::Store(fs::path directory, Identity identity, std::shared_ptr<const Clock> clock)
    : Store(std::move(directory), std::move(identity), std::move(clock), false) {}
Store::Store(fs::path directory, Identity identity, std::shared_ptr<const Clock> clock,
             bool read_only)
    : impl_(std::make_unique<Impl>(std::move(directory), std::move(identity), std::move(clock),
                                   read_only)) {}

Store::~Store() = default;
Store::ResultRead::ResultRead(wire::Task task, fs::path directory, std::string digest,
                              std::uint64_t bytes, bool verify_content, bool include_input)
    : task_(std::move(task)), directory_(std::move(directory)), input_digest_(std::move(digest)),
      input_bytes_(bytes), verify_content_(verify_content), include_input_(include_input) {}
void Store::ResultRead::verify() {
  verified_ = false;
  // Parse the exact bytes whose digest was checked, never a second file read.
  const auto raw =
      payload::read((directory_ / "results" / (std::to_string(task_.attempt()) + ".pb")),
                    task_.result_digest(), payload::max_result_bytes);
  wire::TaskResponse result;
  const bool verify = verify_content_;
  auto task = verify || include_input_
                  ? read_input(task_, directory_ / "input.pb", input_digest_, input_bytes_)
                  : wire::Task{};
  if (!verify && !include_input_)
    payload::check(directory_ / "input.pb", input_digest_, payload::max_input_bytes, input_bytes_);
  if (task_.kind() == wire::DAILY_DOWNLOAD) {
    payload::parse(raw, *result.mutable_daily());

  } else if (task_.kind() == wire::MINUTE_DOWNLOAD) {
    payload::parse(raw, *result.mutable_minutes());

  } else if (task_.kind() == wire::FACTOR) {
    payload::parse(raw, *result.mutable_factor());
    if (verify)
      protocol::validate_factor_result(task.factor(), result.factor());
  } else {
    payload::parse(raw, *result.mutable_backtest());
    if (verify)
      verify_result(task, result.backtest());
  }

  if (include_input_)
    *result.mutable_result_task() = std::move(task);
  response_ = std::move(result);
  verified_ = true;
}
Store::ResultRead Store::prepare_result(const std::string& id) const {
  const auto entry = impl_->find(id);
  if (entry.task.state() != wire::SUCCEEDED)
    throw std::invalid_argument("task has no confirmed result");
  return impl_->prepare_read(entry, true, true);
}
wire::TaskResponse Store::confirm_result(ResultRead read) const {
  if (!read.verified_)
    throw std::invalid_argument("task result has not been verified");
  auto entry = impl_->find(read.task_.id());
  if (read.directory_ != impl_->root / entry.task.id() || entry.task.state() != wire::SUCCEEDED ||
      entry.task.attempt() != read.task_.attempt() ||
      entry.task.result_digest() != read.task_.result_digest() ||
      entry.input_digest != read.input_digest_)
    throw std::invalid_argument("task result changed during verification");
  return std::move(read.response_);
}

Store::Submission::Submission(Impl* owner, fs::path root, wire::Task task)
    : owner_(owner), root_(std::move(root)), task_(std::move(task)) {
  const auto& id = task_.id();
  validate_id(id);
  if (id.find_first_of("/\\") != std::string::npos || id == "." || id == ".." ||
      id == "manager.lock")
    throw std::invalid_argument("invalid task id");
  if (id == "history" || id == "event-segments")
    throw std::invalid_argument("reserved task identifier");
  prepare(task_);
}
Store::Submission Store::submission(const std::string& id,
                                    backtest::v1::BacktestInput input) const {
  wire::Task task;
  task.set_id(id);
  *task.mutable_input() = std::move(input);
  task.set_risk_artifact(risk_providers::Module::selected().artifact());
  return Submission(impl_.get(), impl_->root, std::move(task));
}
Store::Submission Store::submission(const std::string& id, factor::v1::FactorInput input) const {
  wire::Task task;
  task.set_id(id);
  *task.mutable_factor() = std::move(input);
  return Submission(impl_.get(), impl_->root, std::move(task));
}
Store::Submission Store::submission(const data::v1::DownloadAuthorization& authorization) const {
  protocol::validate_message(authorization);
  validate_id(authorization.task_id());
  if (authorization.version() != 2 ||
      authorization.data_instance() != impl_->identity.data_instance ||
      authorization.task_instance() != impl_->identity.instance ||
      authorization.id().size() != 64 ||
      authorization.id().find_first_not_of("0123456789abcdef") != std::string::npos ||
      authorization.provider_artifact().size() != 64 ||
      authorization.provider_artifact().find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("invalid task download authorization");
  wire::Task task;
  task.set_id(authorization.task_id());
  task.set_download_authorization(authorization.id());
  task.set_provider_artifact(authorization.provider_artifact());
  if (authorization.has_daily())
    *task.mutable_daily() = authorization.daily();
  else if (authorization.has_minutes())
    *task.mutable_minutes() = authorization.minutes();
  else
    throw std::invalid_argument("download definition is missing");
  return Submission(impl_.get(), impl_->root, std::move(task));
}

void Store::admit_submission(Submission& input) {
  impl_->writable();
  if (input.owner_ != impl_.get() || input.admitted_)
    throw std::logic_error("invalid task submission admission");
  const auto& id = input.task_.id();
  if (impl_->contains(id)) {
    const auto entry = impl_->find(id);
    input.existing_.emplace();
    *input.existing_->mutable_task() = entry.task;
    input.existing_->set_input_digest(entry.input_digest);
    input.existing_->set_input_bytes(entry.input_bytes);
  } else {
    if (impl_->submitting)
      throw Error(ErrorCode::resource_exhausted, "task submission is already being prepared");
    impl_->admit();
    if (impl_->last_sequence == std::numeric_limits<std::uint32_t>::max())
      throw std::invalid_argument("task submission sequence exhausted");
    input.task_.set_submission_sequence(impl_->last_sequence + 1);
    input.task_.set_submitted_at_ms(impl_->now_ms());
    input.task_.set_updated_at_ms(input.task_.submitted_at_ms());
    impl_->submitting = id;
  }
  input.admitted_ = true;
}
void Store::Submission::prepare_files() {
  if (!admitted_ || prepared_)
    throw std::logic_error("task submission is not awaiting file preparation");
  const auto directory = root_ / task_.id();
  if (existing_) {
    const auto current = read_input(existing_->task(), directory / "input.pb",
                                    existing_->input_digest(), existing_->input_bytes());
    if (current.kind() != task_.kind() ||
        current.download_authorization() != task_.download_authorization() ||
        current.provider_artifact() != task_.provider_artifact() ||
        definition(current).SerializeAsString() != definition(task_).SerializeAsString())
      throw std::invalid_argument("task id already belongs to different input");
  } else {
    safe(directory);
    if (!fs::create_directory(directory))
      throw std::invalid_argument("task directory exists without committed submission");
    created_ = true;
    fs::create_directory(directory / "results");
    if (task_.has_input()) {
      const auto module = risk_providers::Module::selected();
      if (module.artifact() != task_.risk_artifact())
        throw std::invalid_argument("risk plugin artifact changed before capture");
      module.capture(directory);
    }
    sync_directory(directory / "results");
    sync_directory(directory);
    sync_directory(root_);
    const auto stored =
        payload::publish(directory / "input.pb", definition(task_), payload::max_input_bytes);
    digest_ = stored.digest;
    bytes_ = stored.size;
  }
  prepared_ = true;
}
Store::Change Store::register_submission(Submission& input) {
  impl_->writable();
  if (input.owner_ != impl_.get() || !input.prepared_)
    throw std::logic_error("task submission files have not been prepared");
  const auto& task = input.task_;
  if (input.existing_)
    return impl_->unchanged(impl_->find(task.id()));
  if (impl_->submitting != task.id())
    throw std::logic_error("task submission has no reserved capacity");
  auto state = std::make_unique<Change::State>();
  state->owner = impl_.get();
  auto& change = state->pending.emplace();
  change.next = {summary(task), input.digest_, input.bytes_, "", 0};
  change.record = Impl::encode(change.next);
  change.active_entry = Impl::active_entry(change.next);
  change.kind = task.kind() == wire::DAILY_DOWNLOAD    ? "daily.task"
                : task.kind() == wire::MINUTE_DOWNLOAD ? "minutes.task"
                : task.kind() == wire::FACTOR          ? "factor.task"
                                                       : "backtest.task";
  change.manifest = Json{
      {"version", 7},
      {"provider_artifact", task.provider_artifact()},
      {"download_authorization", task.download_authorization()},
      {"risk_artifact", task.risk_artifact()},
      {"submission_sequence", task.submission_sequence()},
      {"submitted_at_ms", task.submitted_at_ms()},
      {"type", change.kind},
      {"input_sha256", input.digest_},
      {"input_bytes", input.bytes_},
      {"id", task.id()}}.dump();
  return Change(std::move(state));
}
bool Store::abandon_submission(Submission& input) {
  if (input.owner_ != impl_.get())
    throw std::logic_error("invalid task submission admission");
  if (input.existing_ || impl_->submitting != input.task_.id())
    return false;
  impl_->submitting.reset();
  if (input.created_) {
    ++impl_->uncommitted;
    impl_->failed = true;
  }
  return input.created_;
}
bool Store::has_submission() const {
  return impl_->submitting.has_value();
}
wire::Task Store::get(const std::string& id) const {
  auto read = prepare_input(id);
  read.load();
  return confirm_input(std::move(read));
}
Store::InputRead::InputRead(wire::Task task, fs::path path, std::string digest, std::uint64_t bytes)
    : task_(std::move(task)), path_(std::move(path)), digest_(std::move(digest)), bytes_(bytes) {}
void Store::InputRead::load() {
  loaded_ = false;
  task_ = read_input(summary(task_), path_, digest_, bytes_);
  loaded_ = true;
}
void Store::InputRead::load_for_claim() {
  load();
  if (task_.has_input() || task_.has_factor()) {
    execution_ = protocol::task_execution(task_, digest_);
    task_.clear_definition(); // Release full evidence on this file worker.
  }
}
wire::TaskAttempt Store::confirm_attempt(InputRead read) const {
  validate_input(read);
  wire::TaskAttempt result;
  if (read.execution_.parameters_case() != wire::TaskExecution::PARAMETERS_NOT_SET) {
    *result.mutable_task() = describe(read.task_.id());
    *result.mutable_execution() = std::move(read.execution_);
  } else {
    *result.mutable_task() = confirm_input(std::move(read));
  }
  return result;
}
Store::InputRead Store::prepare_input(const std::string& id) const {
  const auto entry = impl_->find(id);
  return InputRead(entry.task, impl_->root / id / "input.pb", entry.input_digest,
                   entry.input_bytes);
}
void Store::validate_input(const InputRead& read) const {
  if (!read.loaded_)
    throw std::invalid_argument("task input has not been loaded");
  const auto entry = impl_->find(read.task_.id());
  if (read.path_ != impl_->root / entry.task.id() / "input.pb" ||
      read.digest_ != entry.input_digest || read.bytes_ != entry.input_bytes ||
      read.task_.kind() != entry.task.kind())
    throw std::invalid_argument("task input changed during loading");
}
wire::Task Store::confirm_input(InputRead read) const {
  validate_input(read);
  const auto entry = impl_->find(read.task_.id());
  auto task = entry.task;
  return attach_definition(std::move(task), std::move(read.task_));
}
wire::Task Store::describe(const std::string& id) const {
  return summary(impl_->find(id).task);
}
Store::HistoryRead Store::prepare_history_usage(const std::string& dataset_id) const {
  data::v1::HistoryUsage check;
  check.set_dataset_id(dataset_id);
  (void)protocol::decode_history_usage(check);
  return HistoryRead(impl_->root, dataset_id, impl_->last_sequence);
}
data::v1::HistoryUsage Store::history_usage(const std::string& dataset_id) const {
  auto read = prepare_history_usage(dataset_id);
  while (next_history_page(read))
    read.load_page();
  return read.take();
}
bool Store::next_history_page(HistoryRead& read) const {
  read.page_.clear();
  sqlite::Database::Statement rows(
      *impl_->database,
      "SELECT id FROM tasks WHERE sequence>? AND sequence<=? ORDER BY sequence LIMIT 200");
  rows.bind(1, static_cast<std::int64_t>(read.after_))
      .bind(2, static_cast<std::int64_t>(read.through_));
  while (rows.step()) {
    const auto entry = impl_->find(rows.text(0));
    auto& record = read.page_.emplace_back();
    *record.mutable_task() = entry.task;
    record.set_input_digest(entry.input_digest);
    record.set_input_bytes(entry.input_bytes);
    read.after_ = entry.task.submission_sequence();
  }
  return !read.page_.empty();
}
void Store::HistoryRead::load_page() {
  auto& result = result_;
  const auto& dataset_id = result.dataset_id();
  for (const auto& entry : page_) {
    const auto& id = entry.task().id();
    const auto task = read_input(entry.task(), root_ / id / "input.pb", entry.input_digest(),
                                 entry.input_bytes());
    bool market = false, settlement = false;
    const auto inspect = [&](const data::v1::BarDataset& data) {
      market |= std::ranges::find(data.source_dataset_ids(), dataset_id) !=
                data.source_dataset_ids().end();
      settlement |= std::ranges::find(data.settlement_dataset_ids(), dataset_id) !=
                    data.settlement_dataset_ids().end();
    };
    auto kind = data::v1::HISTORY_REFERENCE_UNSPECIFIED;
    if (task.has_input()) {
      kind = data::v1::HISTORY_BACKTEST;
      for (const auto& contract : task.input().paper().contracts())
        inspect(contract.dataset());
    } else if (task.has_factor()) {
      kind = data::v1::HISTORY_FACTOR;
      for (const auto& series : task.factor().series()) {
        if (series.has_bars())
          inspect(series.bars());
        else
          market |= series.daily().source_dataset_id() == dataset_id;
      }
    }
    const bool output =
        task.history_dataset_id() == dataset_id && (task.has_minutes() || task.has_daily());
    if (!market && !settlement && !output)
      continue;
    auto* row = result.add_references();
    row->set_id(id);
    row->set_kind(output ? data::v1::HISTORY_DOWNLOAD : kind);
    if (market)
      row->add_roles(data::v1::HISTORY_MARKET);
    if (settlement)
      row->add_roles(data::v1::HISTORY_SETTLEMENT);
    if (output)
      row->add_roles(data::v1::HISTORY_OUTPUT);
    if (result.references_size() > 10000)
      throw std::invalid_argument("historical usage exceeds reference limit");
  }
}
std::vector<wire::Task> Store::active_tasks() const {
  return impl_->active_tasks();
}
wire::TaskList Store::list(unsigned limit, unsigned before_sequence) const {
  if (!limit || limit > 200)
    throw std::invalid_argument("task page limit must be 1..200");
  wire::TaskList result;
  auto* capacity = result.mutable_capacity();
  capacity->set_retained_tasks(impl_->last_sequence);
  capacity->set_active_used(impl_->active_count());
  capacity->set_active_reserved(impl_->submitting ? 1 : 0);
  capacity->set_uncommitted(impl_->uncommitted);
  capacity->set_active_limit(1000);
  sqlite::Database::Statement page(
      *impl_->database, "SELECT id FROM tasks WHERE sequence<?1 ORDER BY sequence DESC LIMIT ?2");
  const auto before = before_sequence ? static_cast<std::int64_t>(before_sequence)
                                      : static_cast<std::int64_t>(impl_->last_sequence) + 1;
  page.bind(1, before).bind(2, limit + 1);
  while (page.step()) {
    if (result.tasks_size() == static_cast<int>(limit)) {
      result.set_next_before_sequence(result.tasks(result.tasks_size() - 1).submission_sequence());
      break;
    }
    *result.add_tasks() = summary(impl_->find(page.text(0)).task);
  }
  std::reverse(result.mutable_tasks()->begin(), result.mutable_tasks()->end());
  for (auto& task : active_tasks())
    *result.add_active_tasks() = std::move(task);
  sqlite::Database::Statement counts(
      *impl_->database, "SELECT state, COUNT(*) FROM tasks WHERE state IN (5,7) GROUP BY state");
  while (counts.step()) {
    if (counts.integer(0) == wire::FAILED)
      result.set_failed_count(static_cast<std::uint32_t>(counts.integer(1)));
    else
      result.set_interrupted_count(static_cast<std::uint32_t>(counts.integer(1)));
  }
  return result;
}
wire::TaskLaunches Store::dispatch(const wire::TaskDispatch& processes, bool data_available) const {
  impl_->writable();
  protocol::validate_message(processes);
  if (!processes.has_launch_slots() || processes.launch_slots() > 2)
    throw std::invalid_argument("task dispatch requires an explicit allowance of 0..2 workers");
  std::set<std::string> running;
  for (const auto& id : processes.running()) {
    validate_id(id);
    if (!running.insert(id).second)
      throw std::invalid_argument("duplicate running task identity");
  }
  wire::TaskLaunches result;
  constexpr std::size_t concurrency = 2;
  if (running.size() >= concurrency)
    return result;
  const auto queue = active_tasks();
  for (const auto& task : queue) {
    if (static_cast<unsigned>(result.launches_size()) >= processes.launch_slots() ||
        running.size() + static_cast<std::size_t>(result.launches_size()) >= concurrency)
      break;
    if (task.state() != wire::QUEUED || running.contains(task.id()) || !data_available)
      continue;
    auto* launch = result.add_launches();
    launch->set_task_id(task.id());
    switch (task.kind()) {
    case wire::BACKTEST:
      launch->set_program(wire::BACKTEST_PROGRAM);
      launch->set_risk_artifact(task.risk_artifact());
      break;
    case wire::FACTOR:
      launch->set_program(wire::FACTOR_PROGRAM);
      break;
    case wire::DAILY_DOWNLOAD:
    case wire::MINUTE_DOWNLOAD:
      launch->set_program(wire::DATA_PIPELINE_PROGRAM);
      launch->set_provider_artifact(task.provider_artifact());
      launch->set_minute_download(task.kind() == wire::MINUTE_DOWNLOAD);
      launch->set_daily_download(task.kind() == wire::DAILY_DOWNLOAD);
      break;
    default:
      throw std::invalid_argument("unsupported queued task kind");
    }
  }
  return result;
}
Store::Change Store::claim(const std::string& id) {
  auto read = prepare_input(id);
  read.load();
  return claim(read);
}
Store::Change Store::claim(const InputRead& read) {
  validate_input(read);
  auto entry = impl_->find(read.task_.id());
  if (entry.task.state() != wire::QUEUED || entry.task.attempt() >= 100)
    throw std::invalid_argument("task is not queued or attempt limit reached");
  auto next = summary(entry.task);
  next.set_state(wire::RUNNING);
  next.set_attempt(next.attempt() + 1);
  next.set_completed(0);
  next.clear_error();
  next.clear_result_digest();
  const auto token = unique_process_id();
  return impl_->change(entry, std::move(next), token);
}
Store::Change Store::progress(const std::string& id, const std::string& token, unsigned completed) {
  auto entry = impl_->fenced(id, token);
  if (completed < entry.task.completed() || completed > entry.task.total())
    throw std::invalid_argument("invalid task progress");
  if (completed == entry.task.completed())
    return impl_->unchanged(std::move(entry));
  auto next = summary(entry.task);
  next.set_completed(completed);
  return impl_->change(entry, std::move(next), token);
}
Store::Change Store::cancel(const std::string& id) {
  auto entry = impl_->find(id);
  auto next = summary(entry.task);
  if (next.state() == wire::QUEUED)
    next.set_state(wire::CANCELLED);
  else if (next.state() == wire::RUNNING)
    next.set_state(wire::CANCEL_REQUESTED);
  if (next.state() == entry.task.state())
    return impl_->unchanged(std::move(entry));
  return impl_->change(entry, std::move(next), entry.token);
}
Store::Change Store::retry(const std::string& id) {
  auto entry = impl_->find(id);
  if (entry.task.state() != wire::FAILED && entry.task.state() != wire::INTERRUPTED &&
      entry.task.state() != wire::CANCELLED)
    throw std::invalid_argument("task is not retryable");
  if (entry.task.attempt() >= 100)
    throw std::invalid_argument("task attempt limit reached");
  impl_->admit();
  auto next = summary(entry.task);
  next.set_state(wire::QUEUED);
  next.set_completed(0);
  next.clear_error();
  next.clear_result_digest();
  return impl_->change(entry, std::move(next), "");
}
Store::Completion::Completion(Identity identity, wire::Task task, wire::TaskFinish result,
                              fs::path directory, std::string input_digest,
                              std::uint64_t input_bytes)
    : task_(std::move(task)), directory_(std::move(directory)),
      input_digest_(std::move(input_digest)), input_bytes_(input_bytes), result_(std::move(result)),
      identity_(std::move(identity)) {}
void Store::Completion::prepare_payload() {
  prepared_ = false;
  if (task_.state() != wire::CANCEL_REQUESTED) {
    task_ = read_input(std::move(task_), directory_ / "input.pb", input_digest_, input_bytes_);
    if (result_.has_daily() && task_.kind() == wire::DAILY_DOWNLOAD && task_.has_daily())
      protocol::validate_message(result_.daily());
    else if (result_.has_minutes() && task_.kind() == wire::MINUTE_DOWNLOAD && task_.has_minutes())
      protocol::validate_message(result_.minutes());
    else if (result_.has_result())
      verify_result(task_, result_.result());
    else if (result_.has_factor() && task_.kind() == wire::FACTOR && task_.has_factor())
      protocol::validate_factor_result(task_.factor(), result_.factor());
    else
      throw std::invalid_argument("task result kind does not match its input");
    const google::protobuf::Message* output = nullptr;
    switch (result_.output_case()) {
    case wire::TaskFinish::kDaily:
      output = &result_.daily();
      break;
    case wire::TaskFinish::kMinutes:
      output = &result_.minutes();
      break;
    case wire::TaskFinish::kResult:
      output = &result_.result();
      break;
    case wire::TaskFinish::kFactor:
      output = &result_.factor();
      break;
    default:
      throw std::logic_error("missing verified task output");
    }
    // A prepared file is not a completion decision. Cancellation, lease expiry
    // or a newer attempt can win while this immutable snapshot is being written.
    // Retain such files as unconfirmed evidence, just as after a process crash.
    const auto path = directory_ / "results" / (std::to_string(task_.attempt()) + ".pb");
    result_digest_ = payload::publish(path, *output, payload::max_result_bytes).digest;
  }
  prepared_ = true;
}
Store::Completion Store::prepare_finish(const wire::TaskFinish& result) {
  auto entry = impl_->fenced(result.id(), result.token());
  if (result.output_case() == wire::TaskFinish::OUTPUT_NOT_SET)
    throw std::invalid_argument("missing task result");
  return Completion(impl_->identity, std::move(entry.task), result, impl_->root / result.id(),
                    entry.input_digest, entry.input_bytes);
}
Store::Change Store::finish(Completion completion) {
  if (!completion.prepared_)
    throw std::invalid_argument("task result payload has not been prepared");
  const auto& result = completion.result_;
  // Cancellation, interruption, retry and lease expiry may have happened
  // during verification. A verified result is not authority to bypass them.
  auto entry = impl_->fenced(result.id(), result.token());
  if (entry.task.state() == wire::CANCEL_REQUESTED) {
    return acknowledge_cancel(result.id(), result.token());
  }
  if (result.has_minutes() || result.has_daily())
    throw std::invalid_argument("download completion requires a prepared publication");
  auto next = summary(entry.task);
  next.set_state(wire::SUCCEEDED);
  next.set_completed(next.total());
  next.set_result_digest(completion.result_digest_);
  return impl_->change(entry, std::move(next), entry.token);
}
data::v1::DownloadPreparation Store::Completion::download_preparation() const {
  if (!prepared_ || (!result_.has_daily() && !result_.has_minutes()))
    throw std::invalid_argument("download result has not been verified");
  data::v1::DownloadPreparation preparation;
  auto* identity = preparation.mutable_identity();
  identity->set_data_instance(identity_.data_instance);
  identity->set_task_instance(identity_.instance);
  identity->set_task_id(task_.id());
  identity->set_attempt(task_.attempt());
  auto* record = preparation.mutable_record();
  record->set_version(1);
  if (result_.has_daily()) {
    *record->mutable_daily() = task_.daily();
    *record->mutable_daily_result() = result_.daily();
  } else {
    *record->mutable_minutes() = task_.minutes();
    *record->mutable_minute_result() = result_.minutes();
  }
  return preparation;
}
Store::Change Store::prepare_publication(Completion completion,
                                         const data::v1::PreparedDownload& prepared) {
  const auto& result = completion.result_;
  auto entry = impl_->fenced(result.id(), result.token());
  if (entry.task.state() == wire::CANCEL_REQUESTED) {
    return acknowledge_cancel(result.id(), result.token());
  }
  protocol::validate_message(prepared);
  const auto preparation = completion.download_preparation();
  const auto dataset =
      result.has_daily() ? result.daily().manifest_sha256() : result.minutes().manifest_sha256();
  if (preparation.identity().SerializeAsString() != prepared.identity().SerializeAsString() ||
      sha256_bytes(preparation.SerializeAsString()) != prepared.candidate_digest() ||
      dataset != prepared.dataset_id())
    throw std::invalid_argument("prepared download does not match task completion");
  auto next = summary(entry.task);
  next.set_state(wire::PUBLISHING);
  next.set_completed(next.total());
  next.set_history_dataset_id(dataset);
  auto* publication = next.mutable_publication();
  *publication->mutable_identity() = prepared.identity();
  publication->set_candidate_digest(prepared.candidate_digest());
  publication->set_publication_id(unique_process_id());
  // The immutable result is already durable. This small journal transition
  // alone authorizes Data to expose the prepared candidate.
  next.set_result_digest(completion.result_digest_);
  return impl_->change(entry, std::move(next), "");
}
std::vector<data::v1::DownloadPublication> Store::pending_publications() const {
  std::vector<data::v1::DownloadPublication> result;
  for (const auto& task : active_tasks())
    if (task.state() == wire::PUBLISHING)
      result.push_back(task.publication());
  return result;
}
Store::Change Store::confirm_publication(const data::v1::PublishedDownload& published) {
  protocol::validate_message(published);
  auto entry = impl_->find(published.decision().identity().task_id());
  if ((entry.task.state() != wire::PUBLISHING && entry.task.state() != wire::SUCCEEDED) ||
      !entry.task.has_publication() ||
      entry.task.publication().SerializeAsString() != published.decision().SerializeAsString())
    throw std::invalid_argument("published download does not match task decision");
  data::v1::DownloadPreparation preparation;
  *preparation.mutable_identity() = published.decision().identity();
  *preparation.mutable_record() = published.record();
  if (sha256_bytes(preparation.SerializeAsString()) != published.decision().candidate_digest())
    throw std::invalid_argument("published download evidence changed");
  if (entry.task.state() == wire::SUCCEEDED)
    return impl_->unchanged(std::move(entry));
  auto next = summary(entry.task);
  next.set_state(wire::SUCCEEDED);
  return impl_->change(entry, std::move(next), "");
}

Store::Change Store::fail(const std::string& id, const std::string& token,
                          const std::string& error) {
  auto entry = impl_->fenced(id, token);
  if (error.empty() || error.size() > 1024)
    throw std::invalid_argument("invalid task error");
  auto next = summary(entry.task);
  next.set_state(entry.task.state() == wire::CANCEL_REQUESTED ? wire::CANCELLED : wire::FAILED);
  next.set_error(error);
  return impl_->change(entry, std::move(next), token);
}
Store::Change Store::interrupt(const std::string& id, const std::string& token,
                               const std::string& error) {
  auto entry = impl_->fenced(id, token);
  if (error.empty() || error.size() > 1024)
    throw std::invalid_argument("invalid interruption reason");
  auto next = summary(entry.task);
  next.set_state(wire::INTERRUPTED);
  next.set_error(error);
  return impl_->change(entry, std::move(next), "");
}
Store::Change Store::acknowledge_cancel(const std::string& id, const std::string& token) {
  auto entry = impl_->fenced(id, token);
  if (entry.task.state() != wire::CANCEL_REQUESTED)
    throw std::invalid_argument("cancellation was not requested");
  auto next = summary(entry.task);
  next.set_state(wire::CANCELLED);
  return impl_->change(entry, std::move(next), token);
}
} // namespace asterion::tasks

namespace asterion::tasks {
data::v1::HistoryUsage Store::inspect_history_usage(const fs::path& directory, Identity identity,
                                                    const std::string& id) {
  Store store(directory, std::move(identity), std::make_shared<SystemClock>(), true);
  return store.history_usage(id);
}
} // namespace asterion::tasks
