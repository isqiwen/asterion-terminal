#include "risk_module.hpp"
#include "daily_factor_source.hpp"
#include "bar_dataset_source.hpp"
#include "history_minutes.hpp"
#include "history_daily.hpp"
#include "history_providers.hpp"
#include "history_archive.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <fstream>
#include "task_store.hpp"
#include "engine.hpp"
#include "factor_engine.hpp"
#include "sqlite_database.hpp"
#include <algorithm>
#include <asterion/foundation/decimal.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/protocol/factor.hpp>
#include <map>
#include <set>
#include <stdexcept>
namespace asterion::tasks {
namespace wire = research::v1;
namespace fs = std::filesystem;
namespace {
void safe(const fs::path& path) {
  if (fs::is_symlink(path))
    throw std::invalid_argument("task store rejects symbolic links");
}
// Storage format is versioned independently of the transport. Binary messages
// are encoded as byte arrays here, not placed inside another wire message.
Json bytes(const google::protobuf::Message& value) {
  const auto raw = value.SerializeAsString();
  return Json(std::vector<unsigned char>(raw.begin(), raw.end()));
}
template <class T> T message(const Json& json) {
  if (!json.is_array() || json.size() > 128 * 1024 * 1024)
    throw std::invalid_argument("invalid stored message");
  std::string raw;
  raw.reserve(json.size());
  for (const auto& byte : json) {
    if (!byte.is_number_integer() || byte < 0 || byte > 255)
      throw std::invalid_argument("invalid stored byte");
    raw += static_cast<char>(byte.get<unsigned>());
  }
  T value;
  if (!value.ParseFromString(raw))
    throw std::invalid_argument("invalid persisted Protobuf");
  protocol::validate_message(value);
  return value;
}
void prepare(wire::Task& task) {
  if (task.has_daily_factor()) {
    protocol::validate_daily_factor(task.daily_factor());
    task.set_kind(wire::DAILY_FACTOR);
    task.set_source_name(task.daily_factor().dataset().source_dataset_id());
    task.set_instrument(task.daily_factor().dataset().contract_id());
    task.set_total(static_cast<unsigned>(task.daily_factor().dataset().bars_size()));
  } else if (task.has_daily()) {
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
    factor::validate(task.factor());
    task.set_kind(wire::FACTOR);
    const auto& c = task.factor().dataset().contract();
    task.set_instrument(c.venue() + "/" + c.symbol());
    task.set_source_name(task.factor().dataset().revision());
    task.set_total(static_cast<unsigned>(factor::work_units(task.factor())));
    task.clear_trading_day();
  } else if (task.has_input()) {
    backtest::validate(task.input());
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
  task.set_state(wire::QUEUED);
}
Json definition(const wire::Task& task) {
  if (task.kind() == wire::DAILY_FACTOR && task.has_daily_factor())
    return bytes(task.daily_factor());
  if (task.kind() == wire::DAILY_DOWNLOAD && task.has_daily())
    return history_files::daily_request_json(task.daily());
  if (task.kind() == wire::MINUTE_DOWNLOAD && task.has_minutes())
    return history_files::minute_request_json(task.minutes());
  if (task.kind() == wire::FACTOR && task.has_factor())
    return protocol::decode_factor(task.factor());
  if (task.kind() == wire::BACKTEST && task.has_input())
    return protocol::decode_backtest(task.input());
  throw std::invalid_argument("task kind and input disagree");
}
void transition(const wire::Task& before, const std::string& old_token, const wire::Task& after,
                const std::string& token) {
  const auto a = before.state(), b = after.state();
  const bool start = a == wire::QUEUED && b == wire::RUNNING;
  const bool retry =
      (a == wire::FAILED || a == wire::INTERRUPTED || a == wire::CANCELLED) && b == wire::QUEUED;
  const bool running =
      a == wire::RUNNING && (b == wire::RUNNING || b == wire::CANCEL_REQUESTED ||
                             b == wire::SUCCEEDED || b == wire::FAILED || b == wire::INTERRUPTED);
  const bool cancelling =
      a == wire::CANCEL_REQUESTED &&
      (b == wire::CANCEL_REQUESTED || b == wire::CANCELLED || b == wire::INTERRUPTED);
  if (!(start || retry || running || cancelling || (a == wire::QUEUED && b == wire::CANCELLED)))
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
  } else if (retry || b == wire::INTERRUPTED) {
    if (!token.empty())
      throw std::invalid_argument("inactive attempt retains token");
  } else if (token != old_token)
    throw std::invalid_argument("task token changed mid-attempt");
  if (b == wire::SUCCEEDED) {
    if (after.completed() != after.total() || after.result_digest().size() != 64 ||
        after.result_digest().find_first_not_of("0123456789abcdef") != std::string::npos)
      throw std::invalid_argument("invalid committed result identity");
  } else if (!after.result_digest().empty())
    throw std::invalid_argument("unfinished task has result digest");
  if (after.error().size() > 1024)
    throw std::invalid_argument("task error exceeds limit");
}
void verify_result(const wire::Task& task, const wire::BacktestResult& result,
                   const fs::path& directory) {
  protocol::validate_message(result);
  if (task.kind() != wire::BACKTEST || !task.has_input())
    throw std::invalid_argument("not a backtest task");
  const auto& paper = task.input().paper();
  const auto days = paper.contracts(0).dataset().days_size();
  bool contracts = result.account().contracts_size() == paper.contracts_size();
  for (int c = 0; contracts && c < paper.contracts_size(); ++c)
    contracts = result.account().contracts(c).contract().SerializeAsString() ==
                    paper.contracts(c).dataset().contract().SerializeAsString() &&
                result.account().contracts(c).costs().SerializeAsString() ==
                    protocol::encode_costs(
                        costs_on(protocol::cost_schedule(paper.contracts(c).cost_schedule()),
                                 paper.contracts(c)
                                     .dataset()
                                     .bars(paper.contracts(c).dataset().bars_size() - 1)
                                     .trading_day())
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
  static_cast<void>(backtest::result_json(result));
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
  const auto risk_module = risk_providers::Module::pinned(directory, task.risk_artifact());
  if (backtest::result_json(result) !=
      backtest::result_json(backtest::run(task.input(), {}, {}, &risk_module)))
    throw std::invalid_argument("backtest result does not match input and session policy");
}
bool active(wire::TaskState state) {
  return state == wire::RUNNING || state == wire::CANCEL_REQUESTED;
}
} // namespace
struct Store::Impl {
  struct Entry {
    wire::Task task;
    std::string token;
    // Repeated reads still verify the file digest. Deterministic recomputation
    // is needed only once per confirmed digest in this process.
    mutable std::string verified_digest;
  };
  fs::path root;
  std::unique_ptr<history_files::Archive> archive;
  std::unique_ptr<FileLock> owner;
  // Task index and state history: one row per task, one per state change.
  std::unique_ptr<sqlite::Database> database;
  std::map<std::string, Entry> entries;
  // A submission interrupted after creating its directory but before its index
  // commit leaves the directory; it is not loaded and its ID stays reserved.
  std::set<std::string> uncommitted;
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
  explicit Impl(fs::path directory, std::shared_ptr<const Clock> source, bool inspect)
      : root(std::move(directory)), read_only(inspect), clock(std::move(source)) {
    if (!clock)
      throw std::invalid_argument("task store requires a clock");
    std::uint64_t inspection_bytes = 0, inspection_records = 0;
    const auto inspect_size = [&](std::int64_t bytes, std::int64_t maximum) {
      if (bytes <= 0 || bytes > maximum)
        throw std::invalid_argument("invalid task record size");
      if (read_only &&
          ((inspection_bytes += bytes) > 256 * 1024 * 1024 || ++inspection_records > 100000))
        throw std::invalid_argument("task reference inspection limit exceeded");
    };
    std::set<std::uint32_t> sequences;
    if (!root.is_absolute() || !fs::is_directory(root))
      throw std::invalid_argument("task store requires an existing absolute directory");
    safe(root);
    owner = std::make_unique<FileLock>(root, "manager.lock",
                                       read_only ? FileLock::Access::shared_existing
                                                 : FileLock::Access::exclusive);
    archive = std::make_unique<history_files::Archive>(
        root / "history", read_only ? history_files::Archive::Access::read_only
                                    : history_files::Archive::Access::writer);
    safe(root / "tasks.sqlite");
    safe(root / "tasks.sqlite-wal");
    safe(root / "tasks.sqlite-shm");
    database = std::make_unique<sqlite::Database>(root / "tasks.sqlite",
                                                  read_only ? sqlite::Database::Access::read_only
                                                            : sqlite::Database::Access::writer);
    if (read_only)
      database->execute("BEGIN");
    else {
      sqlite::Database::Transaction schema(*database);
      database->execute("CREATE TABLE IF NOT EXISTS tasks(id TEXT PRIMARY KEY,"
                        " sequence INTEGER NOT NULL UNIQUE, kind TEXT NOT NULL,"
                        " manifest TEXT NOT NULL, state INTEGER NOT NULL,"
                        " attempt INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL) STRICT");
      database->execute("CREATE TABLE IF NOT EXISTS task_events(task_id TEXT NOT NULL"
                        " REFERENCES tasks(id), sequence INTEGER NOT NULL, body TEXT NOT NULL,"
                        " PRIMARY KEY(task_id, sequence)) STRICT");
      schema.commit();
    }
    std::set<std::string> indexed;
    {
      sqlite::Database::Statement ids(*database, "SELECT id FROM tasks");
      while (ids.step()) {
        if (read_only && indexed.size() >= 1000)
          throw std::invalid_argument("task reference inspection limit exceeded");
        indexed.insert(ids.text(0));
      }
    }
    for (const auto& item : fs::directory_iterator(root)) {
      safe(item.path());
      const auto name = item.path().filename().string();
      if ((name == "history" && item.is_directory()) ||
          (item.is_regular_file() &&
           (name == "manager.lock" || name == "tasks.sqlite" || name == "tasks.sqlite-wal" ||
            name == "tasks.sqlite-shm" || name == "tasks.sqlite-journal")))
        continue;
      validate_id(name);
      if (!item.is_directory())
        throw std::invalid_argument("unknown task store entry");
      if (!indexed.contains(name))
        uncommitted.insert(name);
    }
    sqlite::Database::Statement tasks(
        *database, "SELECT id, CASE WHEN length(CAST(manifest AS BLOB)) <= 134217728 THEN manifest "
                   "END, length(CAST(manifest AS BLOB)) FROM tasks ORDER BY sequence");
    while (tasks.step()) {
      const auto id = tasks.text(0);
      validate_id(id);
      if (!fs::is_directory(root / id))
        throw std::invalid_argument("indexed task directory is missing");
      safe(root / id / "results");
      inspect_size(tasks.integer(2), 128 * 1024 * 1024);
      std::vector<Json> records{parse_json(tasks.text(1), 128 * 1024 * 1024)};
      {
        sqlite::Database::Statement events(
            *database,
            "SELECT sequence, CASE WHEN length(CAST(body AS BLOB)) <= 65536 THEN body END, "
            "length(CAST(body AS BLOB)) FROM task_events WHERE task_id=? ORDER BY sequence");
        events.bind(1, id);
        while (events.step()) {
          if (events.integer(0) != static_cast<std::int64_t>(records.size()))
            throw std::invalid_argument("task state history has a gap");
          inspect_size(events.integer(2), 65536);
          records.push_back(parse_json(events.text(1), 65536));
        }
      }
      Entry entry;
      if (!records.front().is_object() || records.front().value("version", 0) != 4)
        throw std::invalid_argument("unsupported task manifest");
      require_fields(records.front(), {"version", "type", "input", "id", "submission_sequence",
                                       "submitted_at_ms", "provider_artifact", "risk_artifact"});
      if (records.front().at("id") != id)
        throw std::invalid_argument("unsupported task manifest");
      const auto& manifest = records.front();
      if (!manifest.at("submission_sequence").is_number_unsigned() ||
          manifest.at("submission_sequence") < 1 || manifest.at("submission_sequence") > 1000 ||
          !manifest.at("submitted_at_ms").is_number_integer() ||
          manifest.at("submitted_at_ms") <= 0 || manifest.at("submitted_at_ms") > 9223372036854LL)
        throw std::invalid_argument("invalid task submission metadata");
      entry.task.set_submission_sequence(manifest.at("submission_sequence").get<std::uint32_t>());
      entry.task.set_submitted_at_ms(manifest.at("submitted_at_ms").get<std::int64_t>());
      entry.task.set_updated_at_ms(entry.task.submitted_at_ms());
      if (!sequences.insert(entry.task.submission_sequence()).second)
        throw std::invalid_argument("duplicate task submission sequence");
      last_sequence = std::max(last_sequence, entry.task.submission_sequence());
      entry.task.set_id(id);
      if (records.front().at("type") == "daily-factor.task")
        *entry.task.mutable_daily_factor() =
            message<wire::DailyFactorInput>(records.front().at("input"));
      else if (records.front().at("type") == "daily.task")
        *entry.task.mutable_daily() = history_files::daily_request(records.front().at("input"));
      else if (records.front().at("type") == "minutes.task")
        *entry.task.mutable_minutes() = history_files::minute_request(records.front().at("input"));
      else if (records.front().at("type") == "backtest.task")
        *entry.task.mutable_input() = protocol::encode_backtest(records.front().at("input"));
      else if (records.front().at("type") == "factor.task")
        *entry.task.mutable_factor() = protocol::encode_factor(records.front().at("input"));
      else
        throw std::invalid_argument("unsupported task manifest type");
      entry.task.set_provider_artifact(manifest.at("provider_artifact").get<std::string>());
      if (entry.task.has_minutes() || entry.task.has_daily()) {
        const auto& hash = entry.task.provider_artifact();
        if (hash.size() != 64 || !std::ranges::all_of(hash, [](char c) {
              return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))
          throw std::invalid_argument("invalid task provider artifact");
      } else if (!entry.task.provider_artifact().empty())
        throw std::invalid_argument("unexpected task provider artifact");
      entry.task.set_risk_artifact(manifest.at("risk_artifact").get<std::string>());
      if (entry.task.has_input()) {
        const auto& hash = entry.task.risk_artifact();
        if (hash.size() != 64 || !std::ranges::all_of(hash, [](char c) {
              return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))
          throw std::invalid_argument("invalid risk plugin artifact");
      } else if (!entry.task.risk_artifact().empty())
        throw std::invalid_argument("unexpected task risk artifact");
      prepare(entry.task);
      for (std::size_t i = 1; i < records.size(); ++i) {
        const auto& event = records[i];
        require_fields(event, {"version", "state", "attempt", "token", "completed", "error",
                               "digest", "updated_at_ms"});
        if (event.at("version") != 2)
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
        const auto previous = entry.task;
        entry.task.set_updated_at_ms(event.at("updated_at_ms").get<std::int64_t>());
        const auto old_token = entry.token;
        entry.task.set_state(static_cast<wire::TaskState>(state));
        entry.task.set_attempt(event.at("attempt").get<unsigned>());
        entry.task.set_completed(completed);
        entry.task.set_error(event.at("error").get<std::string>());
        entry.task.set_result_digest(event.at("digest").get<std::string>());
        entry.token = event.at("token").get<std::string>();
        transition(previous, old_token, entry.task, entry.token);
      }
      entries.emplace(id, std::move(entry));
    }
    if (last_sequence != entries.size())
      throw std::invalid_argument("task submission sequence has missing records");
    if (read_only)
      return;
    for (auto& [id, entry] : entries) {
      (void)id;
      if (active(entry.task.state())) {
        auto next = entry.task;
        next.set_state(wire::INTERRUPTED);
        next.set_error("task service restarted before confirmed completion; "
                       "explicit retry required");
        commit(entry, next, "");
      }
      // Startup checks each result file against its recorded digest only.
      // Deterministic verification (re-reading downloads, recomputing
      // backtests and factors) runs on first read: doing it here made
      // startup proportional to all work ever stored.
      if (entry.task.state() == wire::SUCCEEDED)
        check_result_digest(entry);
    }
  }
  void writable() const {
    if (read_only)
      throw std::logic_error("task store is read only");
    if (failed)
      throw std::runtime_error("task store requires recovery");
  }
  Entry& find(const std::string& id) {
    validate_id(id);
    return entries.at(id);
  }
  void commit(Entry& entry, wire::Task next, const std::string& token) {
    writable();
    next.set_updated_at_ms(now_ms());
    transition(entry.task, entry.token, next, token);
    try {
      const Json event{{"version", 2},
                       {"updated_at_ms", next.updated_at_ms()},
                       {"state", static_cast<int>(next.state())},
                       {"attempt", next.attempt()},
                       {"token", token},
                       {"completed", next.completed()},
                       {"error", next.error()},
                       {"digest", next.result_digest()}};
      sqlite::Database::Transaction transaction(*database);
      sqlite::Database::Statement insert(
          *database, "INSERT INTO task_events VALUES(?, (SELECT COUNT(*) + 1 FROM task_events"
                     " WHERE task_id=?1), ?2)");
      insert.bind(1, next.id()).bind(2, event.dump()).step();
      sqlite::Database::Statement update(
          *database, "UPDATE tasks SET state=?, attempt=?, updated_at_ms=? WHERE id=?");
      update.bind(1, static_cast<std::int64_t>(next.state()))
          .bind(2, static_cast<std::int64_t>(next.attempt()))
          .bind(3, next.updated_at_ms())
          .bind(4, next.id())
          .step();
      transaction.commit();
    } catch (...) {
      failed = true;
      throw;
    }
    entry.task = next;
    entry.token = token;
  }
  Entry& fenced(const std::string& id, const std::string& token) {
    writable();
    auto& entry = find(id);
    if (token.empty() || token != entry.token || !active(entry.task.state()))
      throw std::invalid_argument("stale or inactive task attempt");
    return entry;
  }
  fs::path result_path(const Entry& entry) const {
    return root / entry.task.id() / "results" / (std::to_string(entry.task.attempt()) + ".json");
  }
  void publish(Entry& entry, const google::protobuf::Message& result) {
    const auto path = result_path(entry);
    safe(path);
    safe(path.parent_path());
    if (fs::exists(path))
      throw std::invalid_argument("attempt result already exists without confirmed completion");
    write_file_durably(path, Json{{"version", 1}, {"result", bytes(result)}}.dump());
    auto next = entry.task;
    next.set_state(wire::SUCCEEDED);
    next.set_completed(next.total());
    next.set_result_digest(sha256_file(path));
    auto verified = next.result_digest();
    commit(entry, next, entry.token);
    entry.verified_digest.swap(verified);
  }
  void check_result_digest(const Entry& entry) const {
    const auto path = result_path(entry);
    safe(path);
    safe(path.parent_path());
    if (!fs::is_regular_file(path) || fs::file_size(path) > 64 * 1024 * 1024 ||
        sha256_file(path) != entry.task.result_digest())
      throw std::invalid_argument("task result digest mismatch");
  }
  // `deep` runs the deterministic verification (once per digest per process);
  // without it only the result file's digest is checked.
  wire::TaskResponse read_result(const Entry& entry, bool deep = true) const {
    check_result_digest(entry);
    const auto path = result_path(entry);
    std::ifstream input(path, std::ios::binary);
    const std::vector<Json> records{
        parse_json(std::string(std::istreambuf_iterator<char>(input), {}), 64 * 1024 * 1024)};
    require_fields(records.front(), {"version", "result"});
    if (records.front().at("version") != 1)
      throw std::invalid_argument("unsupported result storage version");
    wire::TaskResponse result;
    const bool verify = deep && !read_only && entry.verified_digest != entry.task.result_digest();
    if (entry.task.kind() == wire::DAILY_FACTOR) {
      *result.mutable_daily_factor() =
          message<wire::DailyFactorResult>(records.front().at("result"));
      if (verify)
        factor::verify_daily_result(entry.task.daily_factor(), result.daily_factor());
    } else if (entry.task.kind() == wire::DAILY_DOWNLOAD) {
      *result.mutable_daily() =
          message<data::v1::DailyDownloadResult>(records.front().at("result"));
      if (verify)
        history_files::verify_daily_result(entry.task.daily(), result.daily());
    } else if (entry.task.kind() == wire::MINUTE_DOWNLOAD) {
      *result.mutable_minutes() =
          message<data::v1::MinuteDownloadResult>(records.front().at("result"));
      if (verify)
        history_files::verify_minute_result(entry.task.minutes(), result.minutes());
    } else if (entry.task.kind() == wire::FACTOR) {
      *result.mutable_factor() = message<wire::FactorResult>(records.front().at("result"));
      if (verify)
        factor::verify_result(entry.task.factor(), result.factor());
    } else {
      // Cached computation never bypasses verification of the owned algorithm bytes.
      if (!verify)
        static_cast<void>(
            risk_providers::Module::pinned(root / entry.task.id(), entry.task.risk_artifact()));
      *result.mutable_backtest() = message<wire::BacktestResult>(records.front().at("result"));
      if (verify)
        verify_result(entry.task, result.backtest(), root / entry.task.id());
    }
    if (verify)
      entry.verified_digest = entry.task.result_digest();
    return result;
  }
};
Store::Store(fs::path directory, std::shared_ptr<const Clock> clock)
    : Store(std::move(directory), std::move(clock), false) {}
Store::Store(fs::path directory, std::shared_ptr<const Clock> clock, bool read_only)
    : impl_(std::make_unique<Impl>(std::move(directory), std::move(clock), read_only)) {
  for (auto& [id, entry] : impl_->entries) {
    if (entry.task.state() != wire::SUCCEEDED ||
        (!entry.task.has_daily() && !entry.task.has_minutes()))
      continue;
    // Already published versions were verified when first published.
    const auto result = impl_->read_result(entry, false);
    data::v1::HistoryRecord record;
    record.set_version(1);
    if (entry.task.has_daily()) {
      *record.mutable_daily() = entry.task.daily();
      *record.mutable_daily_result() = result.daily();
    } else {
      *record.mutable_minutes() = entry.task.minutes();
      *record.mutable_minute_result() = result.minutes();
    }
    if (!read_only)
      impl_->archive->publish(record);
    else {
      // Reference inspection validates the committed result record and source ID,
      // not the existence or integrity of every historical data file.
      data::v1::HistoryUsage identity;
      identity.set_dataset_id(record.has_daily() ? record.daily_result().manifest_sha256()
                                                 : record.minute_result().manifest_sha256());
      (void)protocol::decode_history_usage(identity);
    }
    entry.task.set_history_dataset_id(record.has_daily()
                                          ? record.daily_result().manifest_sha256()
                                          : record.minute_result().manifest_sha256());
  }
}
Store::~Store() = default;
Store::DailyFactorSubmission::DailyFactorSubmission(std::string id,
                                                    wire::DailyFactorRequest request,
                                                    data::v1::HistoryRecord source)
    : id_(std::move(id)), request_(std::move(request)), source_(std::move(source)) {}
void Store::DailyFactorSubmission::verify() {
  verified_ = false;
  protocol::validate_message(request_);
  input_.Clear();
  input_.set_version(1);
  input_.set_lookback(request_.lookback());
  input_.set_horizon(request_.horizon());
  if (request_.has_full_sample())
    input_.set_full_sample(request_.full_sample());
  else if (request_.has_holdout_start())
    input_.set_holdout_start(request_.holdout_start());
  *input_.mutable_dataset() = daily_factor_dataset(source_);
  input_.set_dataset_revision(protocol::daily_factor_revision(input_.dataset()));
  protocol::validate_daily_factor(input_);
  verified_ = true;
}
Store::DailyFactorSubmission Store::prepare_daily_factor(const std::string& id,
                                                         const wire::DailyFactorRequest& request) {
  validate_id(id);
  protocol::validate_message(request);
  validate_id(request.source_dataset_id());
  return DailyFactorSubmission(id, request, impl_->archive->get(request.source_dataset_id()));
}
wire::Task Store::submit(DailyFactorSubmission submission) {
  if (!submission.verified_)
    throw std::invalid_argument("daily factor source has not been verified");
  // Recheck durable identity after disk verification, before accepting a new task.
  if (impl_->archive->get(submission.request_.source_dataset_id()).SerializeAsString() !=
      submission.source_.SerializeAsString())
    throw std::invalid_argument("dataset archive changed during resolution");
  wire::Task task;
  task.set_id(submission.id_);
  *task.mutable_daily_factor() = std::move(submission.input_);
  return submit_task(std::move(task));
}
wire::DailyFactorResult Store::daily_factor_result(const std::string& id) const {
  const auto& entry = impl_->find(id);
  if (entry.task.state() != wire::SUCCEEDED || entry.task.kind() != wire::DAILY_FACTOR)
    throw std::invalid_argument("daily factor result is not confirmed");
  return impl_->read_result(entry).daily_factor();
}
wire::Task Store::submit(const std::string& id, const wire::BacktestInput& input) {
  wire::Task task;
  task.set_id(id);
  *task.mutable_input() = input;
  task.set_risk_artifact(risk_providers::Module::selected().artifact());
  return submit_task(std::move(task));
}
wire::Task Store::submit(const std::string& id, const wire::FactorInput& input) {
  wire::Task task;
  task.set_id(id);
  *task.mutable_factor() = input;
  return submit_task(std::move(task));
}
wire::Task Store::submit_task(wire::Task task) {
  const auto id = task.id();
  validate_id(id);
  if (id.find_first_of("/\\") != std::string::npos || id == "." || id == ".." ||
      id == "manager.lock")
    throw std::invalid_argument("invalid task id");
  prepare(task);
  impl_->writable();
  if (impl_->uncommitted.contains(id))
    throw std::invalid_argument("task directory exists without committed submission");
  if (impl_->entries.contains(id)) {
    const auto current = get(id);
    if (current.kind() != task.kind() || definition(current) != definition(task))
      throw std::invalid_argument("task id already belongs to different input");
    return current;
  }
  if (impl_->entries.size() + impl_->uncommitted.size() >= 1000)
    throw std::invalid_argument("task store capacity reached");
  task.set_submission_sequence(impl_->last_sequence + 1);
  task.set_submitted_at_ms(impl_->now_ms());
  task.set_updated_at_ms(task.submitted_at_ms());
  if (id == "history")
    throw std::invalid_argument("reserved task identifier");
  const auto directory = impl_->root / id;
  safe(directory);
  if (!fs::create_directory(directory))
    throw std::invalid_argument("task directory exists without committed submission");
  try {
    fs::create_directory(directory / "results");
    if (task.has_input()) {
      const auto module = risk_providers::Module::selected();
      if (module.artifact() != task.risk_artifact())
        throw std::invalid_argument("risk plugin artifact changed before capture");
      module.capture(directory);
    }
    Impl::Entry entry;
    const Json manifest{{"version", 4},
                        {"provider_artifact", task.provider_artifact()},
                        {"risk_artifact", task.risk_artifact()},
                        {"submission_sequence", task.submission_sequence()},
                        {"submitted_at_ms", task.submitted_at_ms()},
                        {"type", task.kind() == wire::DAILY_FACTOR      ? "daily-factor.task"
                                 : task.kind() == wire::DAILY_DOWNLOAD  ? "daily.task"
                                 : task.kind() == wire::MINUTE_DOWNLOAD ? "minutes.task"
                                 : task.kind() == wire::FACTOR          ? "factor.task"
                                                                        : "backtest.task"},
                        {"input", definition(task)},
                        {"id", id}};
    sqlite::Database::Transaction transaction(*impl_->database);
    sqlite::Database::Statement insert(*impl_->database,
                                       "INSERT INTO tasks VALUES(?, ?, ?, ?, ?, 0, ?)");
    insert.bind(1, id)
        .bind(2, static_cast<std::int64_t>(task.submission_sequence()))
        .bind(3, manifest.at("type").get<std::string>())
        .bind(4, manifest.dump())
        .bind(5, static_cast<std::int64_t>(task.state()))
        .bind(6, task.submitted_at_ms())
        .step();
    transaction.commit();
    entry.task = std::move(task);
    const auto result = entry.task;
    impl_->entries.emplace(id, std::move(entry));
    impl_->last_sequence = result.submission_sequence();
    return result;
  } catch (...) {
    impl_->failed = true;
    throw;
  }
}
wire::Task Store::submit(const std::string& id, const data::v1::MinuteDownload& input,
                         const std::string& token) {
  // Validate before creating durable state. Credentials live outside task journals.
  auto check = history_providers::minutes(input.source(), token);
  history_providers::validate_request(input.source(), history_files::minute_range(input).instrument,
                                      input.interval_minutes(), input.requests_per_minute());
  wire::Task task;
  task.set_id(id);
  *task.mutable_minutes() = input;
  task.set_provider_artifact(history_providers::artifact(input.source()));
  const bool already_submitted = impl_->entries.contains(id);
  auto result = submit_task(std::move(task));
  if (already_submitted)
    return result; // The first submission owns the immutable credential snapshot.
  const auto secret = impl_->root / id / "provider.credential";
  safe(secret);
  replace_file_durably(secret, token, true);
  return result;
}
wire::Task Store::submit(const std::string& id, const data::v1::DailyDownload& input,
                         const std::string& token) {
  // Validate before creating durable state. Credentials live outside task journals.
  auto check = history_providers::daily(input.source(), token);
  history_providers::validate_request(input.source(), history_files::daily_range(input).instrument,
                                      0, input.requests_per_minute());
  wire::Task task;
  task.set_id(id);
  *task.mutable_daily() = input;
  task.set_provider_artifact(history_providers::artifact(input.source()));
  const bool already_submitted = impl_->entries.contains(id);
  auto result = submit_task(std::move(task));
  if (already_submitted)
    return result; // The first submission owns the immutable credential snapshot.
  const auto secret = impl_->root / id / "provider.credential";
  safe(secret);
  replace_file_durably(secret, token, true);
  return result;
}
void Store::download_attempt(wire::TaskAttempt& attempt) const {
  if (attempt.task().kind() != wire::MINUTE_DOWNLOAD &&
      attempt.task().kind() != wire::DAILY_DOWNLOAD)
    return;
  const auto& entry = impl_->find(attempt.task().id());
  if (entry.task.state() != wire::RUNNING || attempt.token() != entry.token ||
      attempt.task().kind() != entry.task.kind())
    throw std::invalid_argument("inactive download attempt");
  const auto secret = impl_->root / entry.task.id() / "provider.credential";
  safe(secret);
  if (!fs::is_regular_file(secret) || fs::file_size(secret) > 256)
    throw std::invalid_argument("download credential unavailable");
  std::ifstream file(secret, std::ios::binary);
  std::string token(257, '\0');
  file.read(token.data(), token.size());
  token.resize(file.gcount());
  if (!file.eof() || token.size() > 256)
    throw std::invalid_argument("download credential unavailable");
  // The worker validates credentials using the task-pinned provider, which may no longer
  // be enabled in the Task Service catalog. Never substitute the current provider here.
  attempt.set_provider_token(token);
  const auto directory =
      entry.task.has_daily()
          ? impl_->archive->directory(history_files::daily_range(entry.task.daily()).instrument,
                                      entry.task.daily().source(), 0, entry.task.id())
          : impl_->archive->directory(history_files::minute_range(entry.task.minutes()).instrument,
                                      entry.task.minutes().source(),
                                      entry.task.minutes().interval_minutes(), entry.task.id());
  safe(directory);
  fs::create_directory(directory);
  const auto path = directory.u8string();
  attempt.set_output_directory(std::string(path.begin(), path.end()));
}
data::v1::MinuteDownloadResult Store::minute_result(const std::string& id) const {
  const auto& entry = impl_->find(id);
  if (entry.task.state() != wire::SUCCEEDED || entry.task.kind() != wire::MINUTE_DOWNLOAD)
    throw std::invalid_argument("minute download is not complete");
  return impl_->read_result(entry).minutes();
}
data::v1::DailyDownloadResult Store::daily_result(const std::string& id) const {
  const auto& entry = impl_->find(id);
  if (entry.task.state() != wire::SUCCEEDED || entry.task.kind() != wire::DAILY_DOWNLOAD)
    throw std::invalid_argument("daily download is not complete");
  return impl_->read_result(entry).daily();
}
wire::Task Store::get(const std::string& id) const {
  return impl_->find(id).task;
}
data::v1::HistoryUsage Store::history_usage(const std::string& dataset_id) const {
  data::v1::HistoryUsage result;
  result.set_dataset_id(dataset_id);
  (void)protocol::decode_history_usage(result);
  for (const auto& [id, entry] : impl_->entries) {
    const auto& task = entry.task;
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
      kind = data::v1::HISTORY_BAR_FACTOR;
      inspect(task.factor().dataset());
    } else if (task.has_daily_factor()) {
      kind = data::v1::HISTORY_DAILY_FACTOR;
      market = task.daily_factor().dataset().source_dataset_id() == dataset_id;
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
  return result;
}
wire::TaskList Store::list() const {
  wire::TaskList result;
  for (const auto& [id, e] : impl_->entries) {
    (void)id;
    auto* task = result.add_tasks();
    *task = e.task;
    if (task->has_minutes())
      task->set_minute_interval_minutes(task->minutes().interval_minutes());
    if (task->has_daily())
      task->set_data_source(task->daily().source());
    if (task->has_minutes())
      task->set_data_source(task->minutes().source());
    task->clear_definition();
  }
  std::sort(result.mutable_tasks()->begin(), result.mutable_tasks()->end(),
            [](const auto& a, const auto& b) {
              return a.submission_sequence() < b.submission_sequence();
            });
  return result;
}
wire::TaskLaunches Store::dispatch(const wire::TaskDispatch& processes) const {
  impl_->writable();
  protocol::validate_message(processes);
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
  const auto queue = list();
  for (const auto& task : queue.tasks()) {
    if (running.size() + static_cast<std::size_t>(result.launches_size()) >= concurrency)
      break;
    if (task.state() != wire::QUEUED || running.contains(task.id()))
      continue;
    auto* launch = result.add_launches();
    launch->set_task_id(task.id());
    switch (task.kind()) {
    case wire::BACKTEST:
      launch->set_program(wire::BACKTEST_PROGRAM);
      launch->set_risk_artifact(task.risk_artifact());
      break;
    case wire::DAILY_FACTOR:
      launch->set_daily_factor(true);
      launch->set_program(wire::FACTOR_PROGRAM);
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
std::string Store::claim(const std::string& id) {
  auto& entry = impl_->find(id);
  if (entry.task.state() != wire::QUEUED || entry.task.attempt() >= 100)
    throw std::invalid_argument("task is not queued or attempt limit reached");
  auto next = entry.task;
  next.set_state(wire::RUNNING);
  next.set_attempt(next.attempt() + 1);
  next.set_completed(0);
  next.clear_error();
  next.clear_result_digest();
  const auto token = unique_process_id();
  impl_->commit(entry, next, token);
  return token;
}
void Store::progress(const std::string& id, const std::string& token, unsigned completed) {
  auto& entry = impl_->fenced(id, token);
  if (completed < entry.task.completed() || completed > entry.task.total())
    throw std::invalid_argument("invalid task progress");
  if (completed == entry.task.completed())
    return;
  auto next = entry.task;
  next.set_completed(completed);
  impl_->commit(entry, next, token);
}
wire::Task Store::cancel(const std::string& id) {
  auto& entry = impl_->find(id);
  auto next = entry.task;
  if (next.state() == wire::QUEUED)
    next.set_state(wire::CANCELLED);
  else if (next.state() == wire::RUNNING)
    next.set_state(wire::CANCEL_REQUESTED);
  if (next.state() != entry.task.state())
    impl_->commit(entry, next, entry.token);
  return entry.task;
}
wire::Task Store::retry(const std::string& id) {
  auto& entry = impl_->find(id);
  if (entry.task.state() != wire::FAILED && entry.task.state() != wire::INTERRUPTED &&
      entry.task.state() != wire::CANCELLED)
    throw std::invalid_argument("task is not retryable");
  if (entry.task.attempt() >= 100)
    throw std::invalid_argument("task attempt limit reached");
  auto next = entry.task;
  next.set_state(wire::QUEUED);
  next.set_completed(0);
  next.clear_error();
  next.clear_result_digest();
  impl_->commit(entry, next, "");
  return entry.task;
}
Store::Completion::Completion(wire::Task task, wire::TaskFinish result, fs::path directory)
    : task_(std::move(task)), directory_(std::move(directory)), result_(std::move(result)) {}
void Store::Completion::verify() {
  if (task_.state() != wire::CANCEL_REQUESTED) {
    if (result_.has_daily_factor() && task_.kind() == wire::DAILY_FACTOR &&
        task_.has_daily_factor())
      factor::verify_daily_result(task_.daily_factor(), result_.daily_factor());
    else if (result_.has_daily() && task_.kind() == wire::DAILY_DOWNLOAD && task_.has_daily())
      history_files::verify_daily_result(task_.daily(), result_.daily());
    else if (result_.has_minutes() && task_.kind() == wire::MINUTE_DOWNLOAD && task_.has_minutes())
      history_files::verify_minute_result(task_.minutes(), result_.minutes());
    else if (result_.has_result())
      verify_result(task_, result_.result(), directory_);
    else if (result_.has_factor() && task_.kind() == wire::FACTOR && task_.has_factor())
      factor::verify_result(task_.factor(), result_.factor());
    else
      throw std::invalid_argument("task result kind does not match its input");
  }
  verified_ = true;
}
Store::Completion Store::prepare_finish(const wire::TaskFinish& result) {
  auto& entry = impl_->fenced(result.id(), result.token());
  if (result.output_case() == wire::TaskFinish::OUTPUT_NOT_SET)
    throw std::invalid_argument("missing task result");
  if (result.has_daily()) {
    const auto expected = impl_->archive
                              ->directory(history_files::daily_range(entry.task.daily()).instrument,
                                          entry.task.daily().source(), 0, entry.task.id())
                              .u8string();
    if (result.daily().directory() != std::string(expected.begin(), expected.end()))
      throw std::invalid_argument("daily result directory mismatch");
  }
  if (result.has_minutes()) {
    const auto expected =
        impl_->archive
            ->directory(history_files::minute_range(entry.task.minutes()).instrument,
                        entry.task.minutes().source(), entry.task.minutes().interval_minutes(),
                        entry.task.id())
            .u8string();
    if (result.minutes().directory() != std::string(expected.begin(), expected.end()))
      throw std::invalid_argument("minute result directory mismatch");
  }
  return Completion(entry.task, result, impl_->root / entry.task.id());
}
void Store::finish(Completion completion) {
  if (!completion.verified_)
    throw std::invalid_argument("task result has not been verified");
  const auto& result = completion.result_;
  // Cancellation, interruption, retry and lease expiry may have happened
  // during verification. A verified result is not authority to bypass them.
  auto& entry = impl_->fenced(result.id(), result.token());
  if (entry.task.state() == wire::CANCEL_REQUESTED) {
    acknowledge_cancel(result.id(), result.token());
    return;
  }
  if (result.has_daily_factor())
    impl_->publish(entry, result.daily_factor());
  else if (result.has_daily())
    impl_->publish(entry, result.daily());
  else if (result.has_minutes())
    impl_->publish(entry, result.minutes());
  else if (result.has_result())
    impl_->publish(entry, result.result());
  else
    impl_->publish(entry, result.factor());
  if (result.has_minutes() || result.has_daily()) {
    data::v1::HistoryRecord record;
    record.set_version(1);
    if (result.has_minutes()) {
      *record.mutable_minutes() = entry.task.minutes();
      *record.mutable_minute_result() = result.minutes();
    } else {
      *record.mutable_daily() = entry.task.daily();
      *record.mutable_daily_result() = result.daily();
    }
    impl_->archive->publish(record);
    entry.task.set_history_dataset_id(record.has_daily()
                                          ? record.daily_result().manifest_sha256()
                                          : record.minute_result().manifest_sha256());
  }
}
void Store::finish(const std::string& id, const std::string& token,
                   const wire::BacktestResult& result) {
  wire::TaskFinish request;
  request.set_id(id);
  request.set_token(token);
  *request.mutable_result() = result;
  auto completion = prepare_finish(request);
  completion.verify();
  finish(std::move(completion));
}
void Store::finish(const std::string& id, const std::string& token,
                   const wire::FactorResult& result) {
  wire::TaskFinish request;
  request.set_id(id);
  request.set_token(token);
  *request.mutable_factor() = result;
  auto completion = prepare_finish(request);
  completion.verify();
  finish(std::move(completion));
}
void Store::fail(const std::string& id, const std::string& token, const std::string& error) {
  auto& entry = impl_->fenced(id, token);
  if (error.empty() || error.size() > 1024)
    throw std::invalid_argument("invalid task error");
  auto next = entry.task;
  next.set_state(entry.task.state() == wire::CANCEL_REQUESTED ? wire::CANCELLED : wire::FAILED);
  next.set_error(error);
  impl_->commit(entry, next, token);
}
void Store::interrupt(const std::string& id, const std::string& token, const std::string& error) {
  auto& entry = impl_->fenced(id, token);
  if (error.empty() || error.size() > 1024)
    throw std::invalid_argument("invalid interruption reason");
  auto next = entry.task;
  next.set_state(wire::INTERRUPTED);
  next.set_error(error);
  impl_->commit(entry, next, "");
}
void Store::acknowledge_cancel(const std::string& id, const std::string& token) {
  auto& entry = impl_->fenced(id, token);
  if (entry.task.state() != wire::CANCEL_REQUESTED)
    throw std::invalid_argument("cancellation was not requested");
  auto next = entry.task;
  next.set_state(wire::CANCELLED);
  impl_->commit(entry, next, token);
}
wire::BacktestResult Store::result(const std::string& id) const {
  const auto& entry = impl_->find(id);
  if (entry.task.state() != wire::SUCCEEDED)
    throw std::invalid_argument("task has no confirmed result");
  if (entry.task.kind() != wire::BACKTEST)
    throw std::invalid_argument("not a backtest result");
  return impl_->read_result(entry).backtest();
}
wire::FactorResult Store::factor_result(const std::string& id) const {
  const auto& entry = impl_->find(id);
  if (entry.task.state() != wire::SUCCEEDED || entry.task.kind() != wire::FACTOR)
    throw std::invalid_argument("factor result is not confirmed");
  return impl_->read_result(entry).factor();
}
BarDatasetSources Store::prepare_dataset(const data::v1::BarDatasetRequest& request) const {
  protocol::decode_bar_dataset_request(request);
  BarDatasetSources sources;
  sources.request = request;
  for (const auto& id : request.source_dataset_ids())
    sources.sources.push_back(impl_->archive->get(id));
  for (const auto& id : request.settlement_dataset_ids())
    sources.settlements.push_back(impl_->archive->get(id));
  return sources;
}
void Store::confirm_sources(const BarDatasetSources& sources) const {
  const auto confirm = [&](const auto& ids, const auto& records) {
    if (static_cast<std::size_t>(ids.size()) != records.size())
      throw std::invalid_argument("dataset archive changed during resolution");
    for (int i = 0; i < ids.size(); ++i)
      if (impl_->archive->get(ids.Get(i)).SerializeAsString() != records[i].SerializeAsString())
        throw std::invalid_argument("dataset archive changed during resolution");
  };
  confirm(sources.request.source_dataset_ids(), sources.sources);
  confirm(sources.request.settlement_dataset_ids(), sources.settlements);
}
} // namespace asterion::tasks

namespace asterion::tasks {
void append_saved_references(data::v1::HistoryUsage& usage,
                             const data::v1::ResearchDatasets& saved_datasets) {
  for (const auto& saved : saved_datasets.items()) {
    bool market = false, settlement = false;
    for (const auto& selection : saved.selections()) {
      market |= std::ranges::find(selection.source_dataset_ids(), usage.dataset_id()) !=
                selection.source_dataset_ids().end();
      settlement |= std::ranges::find(selection.settlement_dataset_ids(), usage.dataset_id()) !=
                    selection.settlement_dataset_ids().end();
    }
    if (!market && !settlement)
      continue;
    auto* row = usage.add_references();
    row->set_kind(asterion::data::v1::HISTORY_SAVED_DATASET);
    row->set_id(saved.id());
    row->set_name(saved.name());
    if (market)
      row->add_roles(asterion::data::v1::HISTORY_MARKET);
    if (settlement)
      row->add_roles(asterion::data::v1::HISTORY_SETTLEMENT);
  }
  (void)asterion::protocol::decode_history_usage(usage);
}
data::v1::HistoryUsage Store::inspect_history_usage(const fs::path& directory,
                                                    const std::string& id) {
  Store store(directory, std::make_shared<SystemClock>(), true);
  auto usage = store.history_usage(id);
  append_saved_references(usage, store.impl_->archive->research_datasets());
  return usage;
}
} // namespace asterion::tasks
