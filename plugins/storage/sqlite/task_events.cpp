#include "task_events.hpp"
#include <set>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <fstream>
#include <charconv>
namespace asterion::sqlite {
namespace {
constexpr std::uint64_t maximum_event = 65536;
constexpr int segment_events = 1000;
constexpr std::uint64_t segment_bytes = 4 * 1024 * 1024;
constexpr std::uint64_t maximum_segment = segment_bytes + maximum_event + 256 * segment_events;
std::string bytes(const std::filesystem::path& path, std::uint64_t size) {
  if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) || !size ||
      size > maximum_segment || std::filesystem::file_size(path) != size)
    throw std::invalid_argument("invalid task event segment");
  std::ifstream stream(path, std::ios::binary);
  std::string result{std::istreambuf_iterator<char>(stream), {}};
  if (!stream)
    throw std::runtime_error("task event segment read failed");
  return result;
}
} // namespace
TaskEvents::TaskEvents(Database& database, std::filesystem::path directory, bool read_only)
    : database_(database), directory_(std::move(directory)), read_only_(read_only) {
  if (!read_only_) {
    create_directories_durably(directory_);
    database_.execute("CREATE TABLE IF NOT EXISTS task_event_segments("
                      "id INTEGER PRIMARY KEY, sha256 TEXT NOT NULL, bytes INTEGER NOT NULL,"
                      " records INTEGER NOT NULL) STRICT");
  }
  if (!read_only_)
    database_.execute(
        "CREATE TABLE IF NOT EXISTS task_event_members("
        "task_id TEXT NOT NULL, segment INTEGER NOT NULL REFERENCES task_event_segments(id),"
        " PRIMARY KEY(task_id, segment)) STRICT");
  if (!read_only_)
    database_.execute("CREATE INDEX IF NOT EXISTS task_event_members_segment ON "
                      "task_event_members(segment, task_id)");
  if (std::filesystem::is_symlink(directory_) || !std::filesystem::is_directory(directory_))
    throw std::invalid_argument("invalid task event segment directory");
}
std::filesystem::path TaskEvents::path(std::uint64_t id) const {
  return directory_ / (std::to_string(id) + ".pb");
}
task::v1::StoredTaskEventSegment TaskEvents::live() const {
  task::v1::StoredTaskEventSegment result;
  result.set_version(1);
  Database::Statement events(
      database_,
      "SELECT CASE WHEN length(CAST(task_id AS BLOB)) <= 128 THEN task_id END, sequence, "
      "CASE WHEN length(CAST(body AS BLOB)) <= 65536 THEN body END "
      "FROM task_events ORDER BY rowid");
  std::uint64_t total = 0;
  while (events.step()) {
    auto body = events.text(2);
    total += body.size();
    if (events.integer(1) <= 0 || body.empty() || body.size() > maximum_event ||
        total > segment_bytes + maximum_event || result.events_size() >= segment_events)
      throw std::invalid_argument("invalid active task event log");
    auto* item = result.add_events();
    const auto id = events.text(0);
    validate_id(id);
    item->set_task_id(id);
    item->set_sequence(events.integer(1));
    item->set_body(std::move(body));
  }
  return result;
}
void TaskEvents::read(const Visitor& visit) const {
  std::uint64_t last = 0;
  Database::Statement segments(database_, "SELECT id, CASE WHEN length(sha256)=64 THEN sha256 END, "
                                          "bytes, records FROM task_event_segments ORDER BY id");
  const auto consume = [&](const task::v1::StoredTaskEventSegment& segment) {
    protocol::validate_message(segment);
    if (segment.version() != 1 || segment.events_size() > segment_events)
      throw std::invalid_argument("unsupported task event segment");
    for (const auto& event : segment.events()) {
      validate_id(event.task_id());
      if (!event.sequence() || event.body().empty() || event.body().size() > maximum_event)
        throw std::invalid_argument("invalid task event segment");
      visit(event.task_id(), event.sequence(), event.body());
    }
  };
  while (segments.step()) {
    if (segments.integer(0) != static_cast<std::int64_t>(++last))
      throw std::invalid_argument("task event segments have a gap");
    const auto file = path(last);
    const auto raw = bytes(file, segments.integer(2));
    task::v1::StoredTaskEventSegment segment;
    if (sha256_bytes(raw) != segments.text(1) || !segment.ParseFromString(raw) ||
        !segment.events_size() || segment.events_size() != segments.integer(3))
      throw std::invalid_argument("invalid task event segment");
    std::set<std::string> members;
    for (const auto& event : segment.events())
      members.insert(event.task_id());
    Database::Statement index(
        database_, "SELECT task_id FROM task_event_members WHERE segment=? ORDER BY task_id");
    index.bind(1, last);
    for (const auto& id : members)
      if (!index.step() || index.text(0) != id)
        throw std::invalid_argument("task event membership index differs from segment");
    if (index.step())
      throw std::invalid_argument("task event membership index differs from segment");
    consume(segment);
  }
  Database::Statement orphan(database_,
                             "SELECT 1 FROM task_event_members m LEFT JOIN task_event_segments s "
                             "ON s.id=m.segment WHERE s.id IS NULL LIMIT 1");
  if (orphan.step())
    throw std::invalid_argument("task event membership index refers to a missing segment");
  const auto pending = live();
  consume(pending);
  // A file published before the index transaction failed is retained. It may
  // only be the exact next segment of the still-authoritative active rows.
  const auto next = path(last + 1);
  for (const auto& file : std::filesystem::directory_iterator(directory_)) {
    const auto name = file.path().stem().string();
    std::uint64_t id = 0;
    const auto parsed = std::from_chars(name.data(), name.data() + name.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || !id ||
        file.path().filename() != path(id).filename())
      throw std::invalid_argument("invalid task event segment directory");
    if (id <= last)
      continue;
    const auto raw = pending.SerializeAsString();
    if (file.path() != next || pending.events().empty() || bytes(file.path(), raw.size()) != raw)
      throw std::invalid_argument("uncommitted task event segment differs from active log");
  }
}
const task::v1::StoredTaskEventSegment& TaskEvents::segment(std::uint64_t id) const {
  if (cached_id_ == id)
    return cached_segment_;
  Database::Statement index(database_,
                            "SELECT sha256, bytes, records FROM task_event_segments WHERE id=?");
  index.bind(1, id);
  if (!index.step())
    throw std::invalid_argument("task event membership index refers to a missing segment");
  const auto raw = bytes(path(id), index.integer(1));
  task::v1::StoredTaskEventSegment loaded;
  if (sha256_bytes(raw) != index.text(0) || !loaded.ParseFromString(raw) ||
      loaded.events_size() != index.integer(2))
    throw std::invalid_argument("invalid task event segment");
  cached_segment_.Swap(&loaded);
  cached_id_ = id;
  return cached_segment_;
}
void TaskEvents::read_task(const std::string& task, const Visitor& visit) const {
  Database::Statement segments(
      database_, "SELECT segment FROM task_event_members WHERE task_id=? ORDER BY segment");
  segments.bind(1, task);
  while (segments.step())
    for (const auto& event : segment(segments.integer(0)).events())
      if (event.task_id() == task)
        visit(task, event.sequence(), event.body());
  Database::Statement tail(
      database_, "SELECT sequence, body FROM task_events WHERE task_id=? ORDER BY sequence");
  tail.bind(1, task);
  while (tail.step())
    visit(task, tail.integer(0), tail.text(1));
}
void TaskEvents::rotate() {
  if (read_only_)
    throw std::logic_error("task event log is read only");
  std::int64_t count = 0, size = 0, last = 0;
  {
    Database::Statement usage(
        database_, "SELECT COUNT(*), COALESCE(SUM(length(CAST(body AS BLOB))),0) FROM task_events");
    usage.step();
    count = usage.integer(0);
    size = usage.integer(1);
    Database::Statement segments(database_, "SELECT COALESCE(MAX(id),0) FROM task_event_segments");
    segments.step();
    last = segments.integer(0);
  }
  if (count < segment_events && size < static_cast<std::int64_t>(segment_bytes))
    return;
  const auto segment = live();
  const auto raw = segment.SerializeAsString();
  const auto file = path(last + 1);
  if (std::filesystem::exists(file)) {
    if (bytes(file, raw.size()) != raw)
      throw std::invalid_argument("uncommitted task event segment differs from active log");
    sync_file_durably(file);
  } else
    replace_file_durably(file, raw);
  Database::Transaction transaction(database_);
  Database::Statement insert(database_, "INSERT INTO task_event_segments VALUES(?,?,?,?)");
  insert.bind(1, last + 1)
      .bind(2, sha256_bytes(raw))
      .bind(3, static_cast<std::int64_t>(raw.size()))
      .bind(4, count)
      .step();
  database_.execute("INSERT INTO task_event_members SELECT DISTINCT task_id, " +
                    std::to_string(last + 1) + " FROM task_events");
  database_.execute("DELETE FROM task_events");
  transaction.commit();
}
void TaskEvents::append(const std::string& task, std::uint64_t sequence, const std::string& body) {
  if (read_only_)
    throw std::logic_error("task event log is read only");
  if (body.empty() || body.size() > maximum_event)
    throw std::invalid_argument("invalid task state event size");
  Database::Statement insert(database_, "INSERT INTO task_events VALUES(?,?,?)");
  insert.bind(1, task).bind(2, static_cast<std::int64_t>(sequence)).bind(3, body).step();
}
} // namespace asterion::sqlite
