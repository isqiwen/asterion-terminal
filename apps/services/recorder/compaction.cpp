#include "compaction.hpp"
#include "database.hpp"
#include "parquet_engine.hpp"
#include <asterion/base/error.hpp>
#include <asterion/base/serialization.hpp>
#include <asterion/system/durable_file.hpp>
#include <algorithm>
#include <fstream>
#include <regex>
#include <set>

namespace asterion::recording {
namespace {

namespace fs = std::filesystem;

const std::set<std::string> exchanges{"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"};
constexpr auto journal_name = "compaction.json";
constexpr auto merged_suffix = ".parquet.merging";

bool merged(const fs::path& file)
{
    return exchanges.contains(file.stem().string());
}

std::vector<fs::path> parquet_files(const fs::path& folder)
{
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(folder))
        if (entry.is_regular_file() && entry.path().extension() == ".parquet")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    return files;
}

std::string list(const std::vector<fs::path>& files)
{
    std::string out = "[";
    for (const auto& file : files)
        out += (out.size() > 1 ? ", " : "") + parquet::literal(file);
    return out + "]";
}

std::uint64_t count(duckdb::Connection& connection, const std::vector<fs::path>& files)
{
    const auto result = query(connection, "SELECT count(*) FROM read_parquet(" + list(files) + ")");
    return static_cast<std::uint64_t>(result->GetValue(0, 0).GetValue<std::int64_t>());
}

// Deletes the originals the journal names and gives the merged files their
// names. Every step can be repeated, so an interrupted completion is resumed.
void complete(const fs::path& folder)
{
    const auto journal = folder / journal_name;
    if (!fs::exists(journal))
        return;
    std::ifstream input(journal, std::ios::binary);
    const std::string raw{std::istreambuf_iterator<char>(input), {}};
    const auto document = parse_json(raw, 1024 * 1024);
    for (const auto& name : document.at("remove"))
        fs::remove(folder / fs::path(name.get<std::string>()).filename());
    for (const auto& exchange : document.at("merged")) {
        const auto name = exchange.get<std::string>();
        if (!exchanges.contains(name))
            throw Error(ErrorCode::invalid_request, "invalid merge journal " + journal.string());
        if (fs::exists(folder / (name + merged_suffix)))
            fs::rename(folder / (name + merged_suffix), folder / (name + ".parquet"));
    }
    sync_directory(folder);
    fs::remove(journal);
    sync_directory(folder);
}

// Returns the rows merged, or nothing when the folder is merged already.
std::optional<std::pair<std::uint64_t, std::size_t>>
merge(duckdb::Connection& connection, const fs::path& folder, std::stop_token stop)
{
    complete(folder);
    const auto files = parquet_files(folder);
    if (std::all_of(files.begin(), files.end(), merged))
        return std::nullopt;
    const auto rows = count(connection, files);
    const auto found =
        query(connection, "SELECT DISTINCT venue FROM read_parquet(" + list(files) + ")");
    std::vector<std::string> names;
    std::vector<fs::path> outputs;
    for (duckdb::idx_t row = 0; row < found->RowCount(); ++row) {
        const auto name = found->GetValue(0, row).ToString();
        if (!exchanges.contains(name))
            throw Error(ErrorCode::invalid_request, "unknown exchange in " + folder.string());
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "merge stopped");
        const auto output = folder / (name + merged_suffix);
        fs::remove(output);
        query(
            connection,
            "COPY (SELECT * FROM read_parquet(" + list(files) + ") WHERE venue = '" + name
                + "') TO " + parquet::literal(output) + " (FORMAT parquet, COMPRESSION zstd)");
        sync_file_durably(output);
        outputs.push_back(output);
    }
    if (count(connection, outputs) != rows)
        throw Error(ErrorCode::operation_failed, "merged rows differ in " + folder.string());
    nlohmann::json journal{{"remove", nlohmann::json::array()}, {"merged", names}};
    for (const auto& file : files)
        journal["remove"].push_back(file.filename().string());
    write_file_durably(folder / journal_name, journal.dump());
    complete(folder);
    return std::pair{rows, files.size()};
}

std::string beijing(std::chrono::system_clock::time_point now, const char* format)
{
    const auto time = std::chrono::system_clock::to_time_t(now) + 8 * 3600;
    std::tm tm{};
    gmtime_r(&time, &tm);
    char text[16];
    std::strftime(text, sizeof(text), format, &tm);
    return text;
}

std::string day_of(const fs::path& partition)
{
    static const std::regex pattern("trading_day=([0-9]{8})");
    std::smatch match;
    const auto name = partition.filename().string();
    return std::regex_match(name, match, pattern) ? match[1].str() : std::string{};
}

} // namespace

std::vector<std::string>
compactable_days(const fs::path& recordings, std::chrono::system_clock::time_point now)
{
    const auto today = beijing(now, "%Y%m%d");
    const bool closed = beijing(now, "%H%M") >= "1530";
    std::set<std::string> days;
    for (const auto* table : {"ticks", "minutes"}) {
        const auto folder = recordings / table;
        if (!fs::is_directory(folder))
            continue;
        for (const auto& partition : fs::directory_iterator(folder)) {
            const auto day = day_of(partition.path());
            if (day.empty() || day > today || (day == today && !closed))
                continue;
            const auto files = parquet_files(partition.path());
            if (fs::exists(partition.path() / journal_name)
                || !std::all_of(files.begin(), files.end(), merged))
                days.insert(day);
        }
    }
    return {days.begin(), days.end()};
}

Compacted
compact_day(const fs::path& recordings, const std::string& trading_day, std::stop_token stop)
{
    if (!std::regex_match(trading_day, std::regex("[0-9]{8}")))
        throw Error(ErrorCode::invalid_request, "trading day must be YYYYMMDD");
    auto database = open_database("256 MiB");
    duckdb::Connection connection(*database);
    Compacted result{trading_day};
    for (const auto* table : {"ticks", "minutes"}) {
        const auto folder = recordings / table / ("trading_day=" + trading_day);
        if (!fs::is_directory(folder))
            continue;
        if (const auto merged_rows = merge(connection, folder, stop)) {
            (std::string_view(table) == "ticks" ? result.ticks : result.minutes) =
                merged_rows->first;
            result.files_before += merged_rows->second;
            result.files_after += parquet_files(folder).size();
        }
    }
    return result;
}

void finish_compactions(const fs::path& recordings)
{
    for (const auto* table : {"ticks", "minutes"}) {
        const auto folder = recordings / table;
        if (!fs::is_directory(folder))
            continue;
        for (const auto& partition : fs::directory_iterator(folder))
            if (partition.is_directory())
                complete(partition.path());
    }
}

} // namespace asterion::recording
