#include "history_tables.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

namespace asterion::history_files {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr auto invalid = "table refresh observation is missing or corrupt";

bool digest(std::string_view s)
{
    return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

std::string read(const fs::path& path, std::size_t bound)
{
    if (fs::is_symlink(path) || !fs::is_regular_file(path) || fs::file_size(path) > bound)
        throw std::invalid_argument(invalid);
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::invalid_argument(invalid);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string
partition_key(const TableSeries& series, const std::string& exchange, const std::string& period)
{
    return sha256_bytes(
        Json::array({series.table, series.schema, series.source, exchange, period}).dump());
}

std::string operation_key(
    const TableObservationContext& c,
    const TableSeries& series,
    const std::string& exchange,
    const std::string& period)
{
    return sha256_bytes(
        Json::array({"asterion.table.refresh.operation.v1",
                        c.data_instance,
                        c.task_instance,
                        c.task_id,
                        c.attempt,
                        partition_key(series, exchange, period)})
            .dump());
}

void immutable(const fs::path& path, const std::string& body, std::size_t bound)
{
    if (fs::exists(path) || fs::is_symlink(path)) {
        if (read(path, bound) != body)
            throw std::invalid_argument(invalid);
    } else {
        create_directories_durably(path.parent_path());
        replace_file_durably(path, body);
    }
}

std::pair<Json, std::string> document(const fs::path& root, const std::string& id)
{
    if (!digest(id))
        throw std::invalid_argument(invalid);
    const auto bytes = read(root / "observations" / "objects" / (id + ".json"), 16384);
    return {Json::parse(bytes), bytes};
}

} // namespace

TableRefreshObservation
TableStore::observation_unlocked(const std::string& id, const std::string& operation) const
{
    try {
        const auto [j, bytes] = document(root_, id);
        if (j.at("kind") != "explicit_refresh_response_observation"
            || j.at("operation") != operation)
            throw std::invalid_argument(invalid);
        TableRefreshObservation o;
        o.id = id;
        o.operation = operation;
        o.context = {
            j.at("data_instance"),
            j.at("task_instance"),
            j.at("task_id"),
            j.at("attempt"),
            j.at("provider"),
            j.at("provider_artifact"),
            j.at("selected_snapshot"),
            j.at("selected_content")
        };
        const TableSeries series{j.at("table"), j.at("schema"), j.at("source")};
        const std::string exchange = j.at("exchange"), period = j.at("period");
        if (operation_key(o.context, series, exchange, period) != operation)
            throw std::invalid_argument(invalid);
        const auto selected = revision_unlocked(o.context.selected_snapshot);
        const auto matched = revision_unlocked(j.at("revision_snapshot"));
        if (!selected || !matched || selected->series != series || matched->series != series
            || selected->exchange != exchange || matched->exchange != exchange
            || selected->period != period || matched->period != period
            || selected->content != o.context.selected_content
            || matched->content != j.at("normalized_content").get<std::string>()
            || matched->object != j.at("normalized_object").get<std::string>())
            throw std::invalid_argument(invalid);
        o.revision = *matched;
        const std::string hash = j.at("response_sha256");
        if (!digest(hash))
            throw std::invalid_argument(invalid);
        o.response = {
            j.at("response_format"),
            read(root_ / "responses" / "objects" / (hash + ".json"), max_source_response_bytes),
            std::stoll(j.at("received_at_ns").get<std::string>())
        };
        o.recorded_at_ns = std::stoll(j.at("recorded_at_ns").get<std::string>());
        o.unchanged = j.at("unchanged");
        if (o.response.format != "tushare.fut_daily.raw.v1" || o.response.body.empty()
            || o.response.received_at_ns <= 0 || o.recorded_at_ns <= 0
            || o.response.body.size() != j.at("response_bytes").get<std::size_t>())
            throw std::invalid_argument(invalid);
        return o;
    } catch (const std::exception&) {
        throw std::invalid_argument(invalid);
    }
}

std::optional<TableRefreshObservation> TableStore::refresh_observation(
    const TableObservationContext& c,
    const TableSeries& series,
    const std::string& exchange,
    const std::string& period) const
{
    std::lock_guard lock(mutex_);
    const auto op = operation_key(c, series, exchange, period);
    const auto path =
        root_ / "observations" / "partitions" / partition_key(series, exchange, period) / op;
    if (!fs::exists(path) && !fs::is_symlink(path))
        return {};
    return observation_unlocked(read(path, 64), op);
}

TableRefreshObservation TableStore::record_refresh_observation(
    const TableObservationContext& c,
    const TableRevision& revision,
    const SourceResponse& response,
    std::int64_t now,
    bool unchanged)
{
    writable();
    std::lock_guard lock(mutex_);
    if (revision.series.table != "futures.daily" || revision.exchange != "SHFE" || !c.attempt
        || c.data_instance.empty() || c.task_instance.empty() || c.task_id.empty()
        || c.provider.empty() || !digest(c.provider_artifact)
        || response.format != "tushare.fut_daily.raw.v1" || response.body.empty()
        || response.body.size() > max_source_response_bytes || response.received_at_ns <= 0
        || now <= 0)
        throw std::invalid_argument("invalid table refresh observation");
    const auto op = operation_key(c, revision.series, revision.exchange, revision.period);
    const auto path = root_ / "observations" / "partitions"
        / partition_key(revision.series, revision.exchange, revision.period) / op;
    if (fs::exists(path) || fs::is_symlink(path)) {
        const auto held = observation_unlocked(read(path, 64), op);
        if (held.context != c || held.revision.snapshot != revision.snapshot
            || held.response.format != response.format || held.response.body != response.body
            || held.response.received_at_ns != response.received_at_ns)
            throw std::invalid_argument(
                "table refresh observation identity conflicts with recorded response");
        return held; // includes original record time and original receipt outcome
    }
    const auto selected = revision_unlocked(c.selected_snapshot),
               matched = revision_unlocked(revision.snapshot);
    if (!selected || !matched || selected->series != revision.series
        || selected->exchange != revision.exchange || selected->period != revision.period
        || selected->content != c.selected_content || matched->content != revision.content
        || matched->object != revision.object)
        throw std::invalid_argument("invalid table refresh observation");
    const auto raw_hash = sha256_bytes(response.body);
    const Json j = {
        {"kind", "explicit_refresh_response_observation"},
        {"operation", op},
        {"data_instance", c.data_instance},
        {"task_instance", c.task_instance},
        {"task_id", c.task_id},
        {"attempt", c.attempt},
        {"provider", c.provider},
        {"provider_artifact", c.provider_artifact},
        {"selected_snapshot", c.selected_snapshot},
        {"selected_content", c.selected_content},
        {"table", revision.series.table},
        {"schema", revision.series.schema},
        {"source", revision.series.source},
        {"exchange", revision.exchange},
        {"period", revision.period},
        {"revision_snapshot", revision.snapshot},
        {"normalized_content", revision.content},
        {"normalized_object", revision.object},
        {"response_format", response.format},
        {"response_sha256", raw_hash},
        {"response_bytes", response.body.size()},
        {"received_at_ns", std::to_string(response.received_at_ns)},
        {"recorded_at_ns", std::to_string(now)},
        {"unchanged", unchanged}
    };
    const auto bytes = j.dump() + "\n", id = sha256_bytes(bytes);
    immutable(
        root_ / "responses" / "objects" / (raw_hash + ".json"),
        response.body,
        max_source_response_bytes);
    immutable(root_ / "observations" / "objects" / (id + ".json"), bytes, 16384);
    immutable(path, id, 64); // publish last: unreferenced objects are not observations
    return {id, op, c, revision, response, now, unchanged};
}

std::optional<TableRefreshObservation> TableStore::latest_refresh_observation(
    const TableSeries& series,
    const std::string& exchange,
    const std::string& period) const
{
    std::lock_guard lock(mutex_);
    const auto directory =
        root_ / "observations" / "partitions" / partition_key(series, exchange, period);
    if (!fs::exists(directory))
        return {};
    std::optional<std::pair<std::int64_t, std::pair<std::string, std::string>>> latest;
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto op = entry.path().filename().string();
        if (!digest(op))
            throw std::invalid_argument(invalid);
        const auto id = read(entry.path(), 64);
        const auto [j, bytes] = document(root_, id);
        const std::int64_t at = std::stoll(j.at("recorded_at_ns").get<std::string>());
        const auto candidate = std::pair(at, std::pair(id, op));
        if (!latest || candidate > *latest)
            latest = candidate;
    }
    if (!latest)
        return {};
    auto observation = observation_unlocked(latest->second.first, latest->second.second);
    if (observation.revision.series != series || observation.revision.exchange != exchange
        || observation.revision.period != period)
        throw std::invalid_argument(invalid);
    return observation;
}

std::vector<TableObservationContext>
TableStore::snapshot_observers(const std::string& snapshot, std::stop_token stop) const
{
    if (!digest(snapshot))
        throw std::invalid_argument("invalid table snapshot");
    std::lock_guard lock(mutex_);
    const auto base = root_ / "observations/partitions";
    std::vector<TableObservationContext> result;
    if (!fs::exists(base))
        return result;
    unsigned count = 0;
    for (const auto& partition : fs::directory_iterator(base)) {
        if (partition.is_symlink() || !partition.is_directory()
            || !digest(partition.path().filename().string()))
            throw std::invalid_argument(invalid);
        for (const auto& entry : fs::directory_iterator(partition.path())) {
            if (stop.stop_requested())
                throw std::runtime_error("table observer inspection cancelled");
            if (++count > 100000 || entry.is_symlink() || !entry.is_regular_file()
                || !digest(entry.path().filename().string()))
                throw std::invalid_argument(invalid);
            const auto observation =
                observation_unlocked(read(entry.path(), 64), entry.path().filename().string());
            if (observation.context.selected_snapshot == snapshot
                || observation.revision.snapshot == snapshot)
                result.push_back(observation.context);
        }
    }
    return result;
}

} // namespace asterion::history_files
