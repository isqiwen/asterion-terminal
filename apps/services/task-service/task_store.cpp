#include "daily_factor_source.hpp"
#include "calendar.hpp"
#include "minutes.hpp"
#include "daily.hpp"
#include "tushare.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <fstream>
#include "task_store.hpp"
#include "engine.hpp"
#include "factor_engine.hpp"
#include "file_journal.hpp"
#include "pipeline.hpp"
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
  if (!json.is_array() || json.size() > 16 * 1024 * 1024)
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
    task.set_source_name(task.daily_factor().dataset().source_task_id());
    task.set_instrument(task.daily_factor().dataset().ts_code());
    task.set_total(static_cast<unsigned>(task.daily_factor().dataset().bars_size()));
  } else if (task.has_daily()) {
    const auto range = data_pipeline::daily_range(task.daily());
    task.set_kind(wire::DAILY_DOWNLOAD);
    task.set_source_name("Tushare " + task.daily().ts_code());
    task.set_instrument(range.instrument.venue + "/" + range.instrument.symbol);
    task.set_total(data_pipeline::daily_work_units(task.daily()));
  } else if (task.has_minutes()) {
    const auto range = data_pipeline::minute_range(task.minutes());
    task.set_kind(wire::MINUTE_DOWNLOAD);
    task.set_source_name("Tushare " + task.minutes().ts_code());
    task.set_instrument(range.instrument.venue + "/" + range.instrument.symbol);
    task.set_total(static_cast<unsigned>((range.end_ns - range.begin_ns) / 86400000000000LL + 1));
  } else if (task.has_calendar()) {
    static_cast<void>(protocol::decode_calendar_snapshot(task.calendar()));
    task.set_kind(wire::CALENDAR_IMPORT);
    task.set_source_name(task.calendar().source_name());
    task.set_instrument(task.calendar().contract().venue() + "/" +
                        task.calendar().contract().symbol());
    task.set_total(static_cast<unsigned>(task.calendar().contents().size()));
    task.clear_trading_day();
  } else if (task.has_data()) {
    if (task.data().contents().size() > 4 * 1024 * 1024)
      throw std::invalid_argument("uploaded CSV task exceeds 4 MiB");
    static_cast<void>(protocol::decode_csv_snapshot(task.data()));
    task.set_kind(wire::DATA_IMPORT);
    task.set_source_name(task.data().source_name());
    task.set_instrument(task.data().contract().venue() + "/" + task.data().contract().symbol());
    task.set_total(static_cast<unsigned>(task.data().contents().size()));
    task.clear_trading_day();
  } else if (task.has_factor()) {
    factor::validate(task.factor());
    task.set_kind(wire::FACTOR);
    task.set_instrument(task.factor().contract().venue() + "/" + task.factor().contract().symbol());
    task.set_total(static_cast<unsigned>(factor::work_units(task.factor())));
    task.clear_trading_day();
  } else if (task.has_input()) {
    backtest::validate(task.input());
    task.set_kind(wire::BACKTEST);
    task.set_instrument(task.input().paper().contract().venue() + "/" +
                        task.input().paper().contract().symbol());
    task.set_trading_day(task.input().days(0).trading_day());
    if (task.input().days_size() > 1)
      task.set_trading_day(task.trading_day() + " / " +
                           task.input().days().rbegin()->trading_day());
    task.set_total(static_cast<unsigned>(task.input().paper().ticks_size()));
  } else
    throw std::invalid_argument("task requires an explicit input type");
  task.set_state(wire::QUEUED);
}
Json definition(const wire::Task& task) {
  if (task.kind() == wire::DAILY_FACTOR && task.has_daily_factor())
    return bytes(task.daily_factor());
  if (task.kind() == wire::DAILY_DOWNLOAD && task.has_daily())
    return data_pipeline::daily_request_json(task.daily());
  if (task.kind() == wire::MINUTE_DOWNLOAD && task.has_minutes())
    return data_pipeline::minute_request_json(task.minutes());
  if (task.kind() == wire::CALENDAR_IMPORT && task.has_calendar())
    return protocol::decode_calendar_snapshot(task.calendar());
  if (task.kind() == wire::DATA_IMPORT && task.has_data())
    return protocol::decode_csv_snapshot(task.data());
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
void verify_result(const wire::Task& task, const wire::BacktestResult& result) {
  protocol::validate_message(result);
  if (task.kind() != wire::BACKTEST || !task.has_input())
    throw std::invalid_argument("not a backtest task");
  if (result.version() != 3 || result.dataset_revision() != task.input().dataset_revision() ||
      result.engine_version() != protocol::backtest_engine_version ||
      result.account().cursor() != task.total() || result.account().total() != task.total() ||
      result.equity_size() != static_cast<int>(task.total()) + task.input().days_size() ||
      result.settlements_size() != task.input().days_size() ||
      result.account().contract().SerializeAsString() !=
          task.input().paper().contract().SerializeAsString() ||
      result.account().costs().SerializeAsString() !=
          task.input().paper().costs().SerializeAsString() ||
      result.account().risk().SerializeAsString() !=
          task.input().paper().risk().SerializeAsString() ||
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
  if (backtest::result_json(result) != backtest::result_json(backtest::run(task.input())))
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
    std::unique_ptr<FileJournal> journal;
    // Repeated reads still verify the file digest. Deterministic recomputation
    // is needed only once per confirmed digest in this process.
    mutable std::string verified_digest;
  };
  fs::path root;
  std::unique_ptr<FileLock> owner;
  std::map<std::string, Entry> entries;
  bool failed = false;
  std::shared_ptr<const Clock> clock;
  std::uint32_t last_sequence = 0;
  std::int64_t now_ms() const {
    const auto value = clock->utc_now() / 1000000;
    if (value <= 0)
      throw std::invalid_argument("task clock must report a positive UTC time");
    return value;
  }
  explicit Impl(fs::path directory, std::shared_ptr<const Clock> source)
      : root(std::move(directory)), clock(std::move(source)) {
    if (!clock)
      throw std::invalid_argument("task store requires a clock");
    std::set<std::uint32_t> sequences;
    if (!root.is_absolute() || !fs::is_directory(root))
      throw std::invalid_argument("task store requires an existing absolute directory");
    safe(root);
    owner = std::make_unique<FileLock>(root, "manager.lock");
    for (const auto& item : fs::directory_iterator(root)) {
      safe(item.path());
      const auto id = item.path().filename().string();
      if (id == "manager.lock" && item.is_regular_file())
        continue;
      validate_id(id);
      if (!item.is_directory())
        throw std::invalid_argument("unknown task store entry");
      safe(item.path() / "journal");
      safe(item.path() / "results");
      Entry entry;
      entry.journal = std::make_unique<FileJournal>(item.path() / "journal");
      entry.journal->start();
      const auto records = entry.journal->read();
      if (records.empty())
        throw std::invalid_argument("task submission is incomplete; preserve it for inspection");
      require_fields(records.front(),
                     {"version", "type", "input", "id", "submission_sequence", "submitted_at_ms"});
      if (records.front().at("version") != 2 || records.front().at("id") != id)
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
        *entry.task.mutable_daily() = data_pipeline::daily_request(records.front().at("input"));
      else if (records.front().at("type") == "minutes.task")
        *entry.task.mutable_minutes() = data_pipeline::minute_request(records.front().at("input"));
      else if (records.front().at("type") == "backtest.task")
        *entry.task.mutable_input() = protocol::encode_backtest(records.front().at("input"));
      else if (records.front().at("type") == "factor.task")
        *entry.task.mutable_factor() = protocol::encode_factor(records.front().at("input"));
      else if (records.front().at("type") == "calendar.task")
        *entry.task.mutable_calendar() =
            protocol::encode_calendar_snapshot(records.front().at("input"));
      else if (records.front().at("type") == "data.task")
        *entry.task.mutable_data() = protocol::encode_csv_snapshot(records.front().at("input"));
      else
        throw std::invalid_argument("unsupported task manifest type");
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
    for (auto& [id, entry] : entries) {
      (void)id;
      if (active(entry.task.state())) {
        auto next = entry.task;
        next.set_state(wire::INTERRUPTED);
        next.set_error("task service restarted before confirmed completion; "
                       "explicit retry required");
        commit(entry, next, "");
      }
      if (entry.task.state() == wire::SUCCEEDED)
        static_cast<void>(read_result(entry));
    }
  }
  void writable() const {
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
      entry.journal->append({{"version", 2},
                             {"updated_at_ms", next.updated_at_ms()},
                             {"state", static_cast<int>(next.state())},
                             {"attempt", next.attempt()},
                             {"token", token},
                             {"completed", next.completed()},
                             {"error", next.error()},
                             {"digest", next.result_digest()}});
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
    return root / entry.task.id() / "results" / std::to_string(entry.task.attempt());
  }
  void publish(Entry& entry, const google::protobuf::Message& result) {
    const auto path = result_path(entry);
    safe(path);
    safe(path.parent_path());
    if (!fs::create_directory(path))
      throw std::invalid_argument("attempt result already exists without confirmed completion");
    FileJournal journal(path);
    journal.start();
    journal.append({{"version", 1}, {"result", bytes(result)}});
    auto next = entry.task;
    next.set_state(wire::SUCCEEDED);
    next.set_completed(next.total());
    next.set_result_digest(sha256_file(path / "00000000.json"));
    auto verified = next.result_digest();
    commit(entry, next, entry.token);
    entry.verified_digest.swap(verified);
  }
  wire::TaskResponse read_result(const Entry& entry) const {
    const auto path = result_path(entry);
    safe(path);
    safe(path.parent_path());
    FileJournal journal(path);
    journal.start();
    const auto records = journal.read();
    if (records.size() != 1 || sha256_file(path / "00000000.json") != entry.task.result_digest())
      throw std::invalid_argument("task result digest mismatch");
    require_fields(records.front(), {"version", "result"});
    if (records.front().at("version") != 1)
      throw std::invalid_argument("unsupported result storage version");
    wire::TaskResponse result;
    const bool verify = entry.verified_digest != entry.task.result_digest();
    if (entry.task.kind() == wire::DAILY_FACTOR) {
      *result.mutable_daily_factor() =
          message<wire::DailyFactorResult>(records.front().at("result"));
      if (verify)
        factor::verify_daily_result(entry.task.daily_factor(), result.daily_factor());
    } else if (entry.task.kind() == wire::DAILY_DOWNLOAD) {
      *result.mutable_daily() =
          message<data::v1::DailyDownloadResult>(records.front().at("result"));
      if (verify)
        data_pipeline::verify_daily_result(entry.task.daily(), result.daily());
    } else if (entry.task.kind() == wire::MINUTE_DOWNLOAD) {
      *result.mutable_minutes() =
          message<data::v1::MinuteDownloadResult>(records.front().at("result"));
      if (verify)
        data_pipeline::verify_minute_result(entry.task.minutes(), result.minutes());
    } else if (entry.task.kind() == wire::CALENDAR_IMPORT) {
      *result.mutable_calendar_publication() =
          message<data::v1::CalendarPublication>(records.front().at("result"));
      if (verify)
        data_pipeline::verify_calendar_result(entry.task.calendar(), result.calendar_publication());
    } else if (entry.task.kind() == wire::DATA_IMPORT) {
      *result.mutable_publication() =
          message<data::v1::DatasetPublication>(records.front().at("result"));
      if (verify)
        data_pipeline::verify_result(entry.task.data(), result.publication());
    } else if (entry.task.kind() == wire::FACTOR) {
      *result.mutable_factor() = message<wire::FactorResult>(records.front().at("result"));
      if (verify)
        factor::verify_result(entry.task.factor(), result.factor());
    } else {
      *result.mutable_backtest() = message<wire::BacktestResult>(records.front().at("result"));
      if (verify)
        verify_result(entry.task, result.backtest());
    }
    if (verify)
      entry.verified_digest = entry.task.result_digest();
    return result;
  }
};
Store::Store(fs::path directory, std::shared_ptr<const Clock> clock)
    : impl_(std::make_unique<Impl>(std::move(directory), std::move(clock))) {}
Store::~Store() = default;
Store::DailyFactorSubmission::DailyFactorSubmission(std::string id,
                                                    wire::DailyFactorRequest request,
                                                    wire::Task source,
                                                    data::v1::DailyDownloadResult result)
    : id_(std::move(id)), request_(std::move(request)), source_(std::move(source)),
      result_(std::move(result)) {}
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
  *input_.mutable_dataset() = daily_factor_dataset(source_, result_);
  input_.set_dataset_revision(protocol::daily_factor_revision(input_.dataset()));
  protocol::validate_daily_factor(input_);
  verified_ = true;
}
Store::DailyFactorSubmission Store::prepare_daily_factor(const std::string& id,
                                                         const wire::DailyFactorRequest& request) {
  validate_id(id);
  protocol::validate_message(request);
  validate_id(request.source_task_id());
  return DailyFactorSubmission(id, request, get(request.source_task_id()),
                               daily_result(request.source_task_id()));
}
wire::Task Store::submit(DailyFactorSubmission submission) {
  if (!submission.verified_)
    throw std::invalid_argument("daily factor source has not been verified");
  // Recheck durable identity after disk verification, before accepting a new task.
  if (get(submission.source_.id()).SerializeAsString() != submission.source_.SerializeAsString())
    throw std::invalid_argument("daily factor source task changed during verification");
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
  return submit_task(std::move(task));
}
wire::Task Store::submit(const std::string& id, const wire::FactorInput& input) {
  wire::Task task;
  task.set_id(id);
  *task.mutable_factor() = input;
  return submit_task(std::move(task));
}
wire::Task Store::submit(const std::string& id, const data::v1::CsvSnapshot& input) {
  wire::Task task;
  task.set_id(id);
  *task.mutable_data() = input;
  return submit_task(std::move(task));
}
wire::Task Store::submit(const std::string& id, const data::v1::CalendarCsvSnapshot& input) {
  wire::Task task;
  task.set_id(id);
  *task.mutable_calendar() = input;
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
  if (impl_->entries.contains(id)) {
    const auto current = get(id);
    if (current.kind() != task.kind() || definition(current) != definition(task))
      throw std::invalid_argument("task id already belongs to different input");
    return current;
  }
  if (impl_->entries.size() >= 1000)
    throw std::invalid_argument("task store capacity reached");
  task.set_submission_sequence(impl_->last_sequence + 1);
  task.set_submitted_at_ms(impl_->now_ms());
  task.set_updated_at_ms(task.submitted_at_ms());
  const auto directory = impl_->root / id;
  safe(directory);
  if (!fs::create_directory(directory))
    throw std::invalid_argument("task directory exists without committed submission");
  try {
    fs::create_directory(directory / "journal");
    fs::create_directory(directory / "results");
    Impl::Entry entry;
    entry.journal = std::make_unique<FileJournal>(directory / "journal");
    entry.journal->start();
    entry.journal->append({{"version", 2},
                           {"submission_sequence", task.submission_sequence()},
                           {"submitted_at_ms", task.submitted_at_ms()},
                           {"type", task.kind() == wire::DAILY_FACTOR      ? "daily-factor.task"
                                    : task.kind() == wire::DAILY_DOWNLOAD  ? "daily.task"
                                    : task.kind() == wire::MINUTE_DOWNLOAD ? "minutes.task"
                                    : task.kind() == wire::CALENDAR_IMPORT ? "calendar.task"
                                    : task.kind() == wire::DATA_IMPORT     ? "data.task"
                                    : task.kind() == wire::FACTOR          ? "factor.task"
                                                                           : "backtest.task"},
                           {"input", definition(task)},
                           {"id", id}});
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
  tushare::Minutes check(token);
  (void)data_pipeline::minute_range(input);
  wire::Task task;
  task.set_id(id);
  *task.mutable_minutes() = input;
  auto result = submit_task(std::move(task));
  const auto secret = impl_->root / id / "tushare.token";
  safe(secret);
  replace_file_durably(secret, token, true);
  return result;
}
wire::Task Store::submit(const std::string& id, const data::v1::DailyDownload& input,
                         const std::string& token) {
  // Validate before creating durable state. Credentials live outside task journals.
  tushare::Daily check(token);
  (void)data_pipeline::daily_range(input);
  wire::Task task;
  task.set_id(id);
  *task.mutable_daily() = input;
  auto result = submit_task(std::move(task));
  const auto secret = impl_->root / id / "tushare.token";
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
  const auto secret = impl_->root / entry.task.id() / "tushare.token";
  safe(secret);
  if (!fs::is_regular_file(secret) || fs::file_size(secret) > 256)
    throw std::invalid_argument("download credential unavailable");
  std::ifstream file(secret, std::ios::binary);
  std::string token(257, '\0');
  file.read(token.data(), token.size());
  token.resize(file.gcount());
  if (!file.eof() || token.size() > 256)
    throw std::invalid_argument("download credential unavailable");
  if (entry.task.kind() == wire::DAILY_DOWNLOAD) {
    tushare::Daily check(token);
  } else {
    tushare::Minutes check(token);
  }
  attempt.set_provider_token(token);
  const auto directory = impl_->root / entry.task.id() /
                         (entry.task.kind() == wire::DAILY_DOWNLOAD ? "daily" : "minutes");
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
wire::TaskList Store::list() const {
  wire::TaskList result;
  for (const auto& [id, e] : impl_->entries) {
    (void)id;
    auto* task = result.add_tasks();
    *task = e.task;
    if (task->has_minutes())
      task->set_minute_interval_minutes(task->minutes().interval_minutes());
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
    case wire::DATA_IMPORT:
    case wire::CALENDAR_IMPORT:
      launch->set_program(wire::DATA_PIPELINE_PROGRAM);
      launch->set_settlement_calendar(task.kind() == wire::CALENDAR_IMPORT);
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
Store::Completion::Completion(wire::Task task, wire::TaskFinish result)
    : task_(std::move(task)), result_(std::move(result)) {}
void Store::Completion::verify() {
  if (task_.state() != wire::CANCEL_REQUESTED) {
    if (result_.has_daily_factor() && task_.kind() == wire::DAILY_FACTOR &&
        task_.has_daily_factor())
      factor::verify_daily_result(task_.daily_factor(), result_.daily_factor());
    else if (result_.has_daily() && task_.kind() == wire::DAILY_DOWNLOAD && task_.has_daily())
      data_pipeline::verify_daily_result(task_.daily(), result_.daily());
    else if (result_.has_minutes() && task_.kind() == wire::MINUTE_DOWNLOAD && task_.has_minutes())
      data_pipeline::verify_minute_result(task_.minutes(), result_.minutes());
    else if (result_.has_result())
      verify_result(task_, result_.result());
    else if (result_.has_factor() && task_.kind() == wire::FACTOR && task_.has_factor())
      factor::verify_result(task_.factor(), result_.factor());
    else if (result_.has_publication() && task_.kind() == wire::DATA_IMPORT && task_.has_data())
      data_pipeline::verify_result(task_.data(), result_.publication());
    else if (result_.has_calendar_publication() && task_.kind() == wire::CALENDAR_IMPORT &&
             task_.has_calendar())
      data_pipeline::verify_calendar_result(task_.calendar(), result_.calendar_publication());
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
    const auto expected = (impl_->root / entry.task.id() / "daily").u8string();
    if (result.daily().directory() != std::string(expected.begin(), expected.end()))
      throw std::invalid_argument("daily result directory mismatch");
  }
  if (result.has_minutes()) {
    const auto expected = (impl_->root / entry.task.id() / "minutes").u8string();
    if (result.minutes().directory() != std::string(expected.begin(), expected.end()))
      throw std::invalid_argument("minute result directory mismatch");
  }
  return Completion(entry.task, result);
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
  else if (result.has_factor())
    impl_->publish(entry, result.factor());
  else if (result.has_publication())
    impl_->publish(entry, result.publication());
  else
    impl_->publish(entry, result.calendar_publication());
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
void Store::finish(const std::string& id, const std::string& token,
                   const data::v1::DatasetPublication& result) {
  wire::TaskFinish request;
  request.set_id(id);
  request.set_token(token);
  *request.mutable_publication() = result;
  auto completion = prepare_finish(request);
  completion.verify();
  finish(std::move(completion));
}
void Store::finish(const std::string& id, const std::string& token,
                   const data::v1::CalendarPublication& result) {
  wire::TaskFinish request;
  request.set_id(id);
  request.set_token(token);
  *request.mutable_calendar_publication() = result;
  auto completion = prepare_finish(request);
  completion.verify();
  finish(std::move(completion));
}
data::v1::CalendarPublication Store::calendar_publication(const std::string& id) const {
  const auto& entry = impl_->find(id);
  if (entry.task.state() != wire::SUCCEEDED || entry.task.kind() != wire::CALENDAR_IMPORT)
    throw std::invalid_argument("calendar publication is not complete");
  return impl_->read_result(entry).calendar_publication();
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
data::v1::DatasetPublication Store::publication(const std::string& id) const {
  const auto& entry = impl_->find(id);
  if (entry.task.state() != wire::SUCCEEDED || entry.task.kind() != wire::DATA_IMPORT)
    throw std::invalid_argument("data publication is not complete");
  return impl_->read_result(entry).publication();
}
} // namespace asterion::tasks
