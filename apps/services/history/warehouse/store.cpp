#include "store.hpp"
#include "parquet_engine.hpp"
#include <asterion/base/error.hpp>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/system/process/child.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <regex>
#include <set>

namespace asterion::history::warehouse {
namespace fs = std::filesystem;

namespace {

using K = Kind;
using Json = nlohmann::json;
constexpr std::size_t manifest_limit = 32 * 1024 * 1024;
constexpr Column daily[] = {
    {"exchange", K::text},
    {"product", K::text},
    {"contract", K::text},
    {"day", K::date},
    {"open", K::number},
    {"high", K::number},
    {"low", K::number},
    {"close", K::number},
    {"settlement", K::number},
    {"previous_close", K::number},
    {"previous_settlement", K::number},
    {"volume", K::number},
    {"amount", K::number},
    {"open_interest", K::number},
    {"open_interest_change", K::number},
    {"delivery_settlement", K::number}
};
constexpr Column calendar[] = {
    {"exchange", K::text},
    {"day", K::date},
    {"open", K::boolean},
    {"previous_trading_day", K::date}
};
constexpr Column contracts[] = {
    {"exchange", K::text},
    {"contract", K::text},
    {"product", K::text},
    {"delivery_month", K::text},
    {"source_instrument", K::text},
    {"name", K::text},
    {"list_date", K::date},
    {"delist_date", K::date},
    {"last_delivery_date", K::date},
    {"multiplier", K::number},
    {"per_unit", K::number},
    {"trade_unit", K::text},
    {"quote_unit", K::text},
    {"tick_description", K::text},
    {"delivery_description", K::text},
    {"trading_hours", K::text}
};
constexpr Column minutes[] = {
    {"exchange", K::text},
    {"product", K::text},
    {"contract", K::text},
    {"day", K::date},
    {"start_ms", K::integer},
    {"partial", K::boolean},
    {"open", K::number},
    {"high", K::number},
    {"low", K::number},
    {"close", K::number},
    {"volume", K::number},
    {"average_price", K::number},
    {"open_interest", K::number}
};
constexpr Column settlement[] = {
    {"exchange", K::text},
    {"product", K::text},
    {"contract", K::text},
    {"day", K::date},
    {"long_margin_rate", K::number},
    {"short_margin_rate", K::number},
    {"fee_rate", K::number},
    {"fee_per_lot", K::number},
    {"close_today_fee_rate", K::number},
    {"close_today_fee_per_lot", K::number}
};
constexpr Column price_ticks[] = {
    {"exchange", K::text},
    {"product", K::text},
    {"contract", K::text},
    {"effective_from", K::date},
    {"effective_until", K::date},
    {"price_tick", K::price},
    {"source_url", K::text}
};

duckdb::LogicalType logical(Kind kind)
{
    switch (kind) {
        case K::text:
            return duckdb::LogicalType::VARCHAR;
        case K::date:
            return duckdb::LogicalType::DATE;
        case K::boolean:
            return duckdb::LogicalType::BOOLEAN;
        case K::integer:
            return duckdb::LogicalType::BIGINT;
        case K::number:
            return duckdb::LogicalType::DECIMAL(28, 6);
        case K::price:
            return duckdb::LogicalType::DECIMAL(28, 8);
    }
    throw std::logic_error("unknown warehouse column kind");
}

duckdb::Value value(const Column& column, const Cell& cell)
{
    const auto type = logical(column.kind);
    if (std::holds_alternative<std::monostate>(cell))
        return duckdb::Value(type);
    if (column.kind == K::boolean) {
        if (const auto* flag = std::get_if<bool>(&cell))
            return duckdb::Value::BOOLEAN(*flag);
    } else if (column.kind == K::integer) {
        if (const auto* integer = std::get_if<std::int64_t>(&cell))
            return duckdb::Value::BIGINT(*integer);
    } else {
        std::string text;
        if (const auto* input = std::get_if<std::string>(&cell))
            text = *input;
        else if ((column.kind == K::number || column.kind == K::price)
            && std::holds_alternative<Decimal>(cell))
            text = std::get<Decimal>(cell).str();
        else
            throw std::invalid_argument(
                "warehouse cell type differs from " + std::string(column.name));
        if (column.kind == K::date)
            require_day(text);
        if (column.kind == K::number || column.kind == K::price) {
            const std::size_t scale = column.kind == K::price ? 8 : 6;
            // DuckDB casts can round. Refuse lossy input before asking it to
            // store the exact 128-bit decimal; never pass through double.
            static const std::regex syntax(R"(-?[0-9]+(\.[0-9]+)?)");
            if (!std::regex_match(text, syntax))
                throw std::invalid_argument("warehouse number must be exact decimal text");
            const auto dot = text.find('.');
            if (dot != std::string::npos) {
                while (text.back() == '0')
                    text.pop_back();
                if (text.back() == '.')
                    text.pop_back();
                else if (text.size() - dot - 1 > scale)
                    throw std::invalid_argument(
                        "warehouse number exceeds " + std::to_string(scale) + " decimal places");
            }
            const auto begin = text.front() == '-' ? 1u : 0u;
            const auto end = text.find('.');
            const auto first = text.find_first_not_of('0', begin);
            const auto whole = end == std::string::npos ? text.size() : end;
            if (first != std::string::npos && first < whole && whole - first > 28 - scale)
                throw std::invalid_argument(
                    "warehouse number exceeds DECIMAL(28," + std::to_string(scale) + ")");
        }
        return duckdb::Value(text).DefaultCastAs(type, true);
    }
    throw std::invalid_argument("warehouse cell type differs from " + std::string(column.name));
}

fs::path contained(const fs::path& root, const fs::path& file)
{
    const auto relative = fs::weakly_canonical(file).lexically_relative(fs::weakly_canonical(root));
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        throw Error(ErrorCode::invalid_request, "dataset path leaves its warehouse");
    return file;
}

std::string read_text(const fs::path& root, const fs::path& path, std::uintmax_t limit)
{
    contained(root, path);
    if (!fs::exists(path))
        throw Error(ErrorCode::not_found, "dataset file is missing: " + path.string());
    if (!fs::is_regular_file(path))
        throw std::runtime_error("dataset file is not regular: " + path.string());
    const auto size = fs::file_size(path);
    if (size > limit)
        throw Error(ErrorCode::resource_exhausted, "dataset manifest is too large");
    std::ifstream input(path, std::ios::binary);
    std::string content(size, '\0');
    if (!input || !input.read(content.data(), static_cast<std::streamsize>(size)))
        throw std::runtime_error("cannot read dataset file: " + path.string());
    return content;
}

Json entry(const Partition& part)
{
    Json item{{"file", part.file}, {"sha256", part.sha256}, {"rows", part.rows}};
    if (!part.days.empty())
        item["days"] = part.days;
    if (!part.source.empty())
        item["source"] = part.source;
    return item;
}

} // namespace

// A partition's shard: its table, exchange and month, e.g. minutes/SHFE/2026-10.
std::string shard_of(const std::string& key)
{
    const auto table = key.find('/');
    const auto exchange = key.find('/', table + 1);
    if (exchange == std::string::npos)
        return key;
    const auto end = key.find('/', exchange + 1);
    return key.substr(0, exchange + 1)
        + key.substr(exchange + 1, std::min<std::size_t>(7, end - exchange - 1));
}

void require_day(std::string_view day)
{
    static const std::regex syntax(R"(\d{4}-\d{2}-\d{2})");
    const std::string value(day);
    if (!std::regex_match(value, syntax))
        throw std::invalid_argument("warehouse date must be YYYY-MM-DD");
    const std::chrono::year_month_day date{
        std::chrono::year(std::stoi(value.substr(0, 4))),
        std::chrono::month(std::stoul(value.substr(5, 2))),
        std::chrono::day(std::stoul(value.substr(8, 2)))
    };
    if (!date.ok() || date.year() == std::chrono::year(0))
        throw std::invalid_argument("invalid warehouse date");
}

std::span<const Column> columns(std::string_view table)
{
    if (table == "daily")
        return daily;
    if (table == "calendar")
        return calendar;
    if (table == "contracts")
        return contracts;
    if (table == "minutes")
        return minutes;
    if (table == "settlement")
        return settlement;
    if (table == "price_ticks")
        return price_ticks;
    throw std::invalid_argument("unknown warehouse table");
}

void require_provider(std::string_view provider)
{
    static const std::regex syntax("[a-z][a-z0-9_-]{0,31}");
    if (!std::regex_match(std::string(provider), syntax))
        throw std::invalid_argument("invalid dataset provider");
}

void require_identity(std::string_view identity)
{
    if (identity.size() != 64 || !std::all_of(identity.begin(), identity.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        throw std::invalid_argument("dataset version must be a lowercase SHA-256 identity");
}

void require_key(std::string_view key)
{
    static const std::regex syntax("[A-Za-z0-9_-]+(/[A-Za-z0-9_.-]+)+");
    if (key.size() > 512 || !std::regex_match(std::string(key), syntax))
        throw std::invalid_argument("invalid dataset partition key");
    for (const auto& component : fs::path(key))
        if (component == "." || component == "..")
            throw std::invalid_argument("invalid partition component");
    columns(key.substr(0, key.find('/')));
}

fs::path source_directory(const fs::path& directory, std::string_view provider)
{
    require_provider(provider);
    return provider == "tushare" ? directory : directory / "sources" / provider;
}

Store::Store(fs::path directory, std::string provider)
    : root_(std::move(directory) / "warehouse"),
      provider_(std::move(provider))
{
    require_provider(provider_);
    if (!root_.is_absolute())
        throw std::invalid_argument("warehouse requires an absolute directory");
}

std::string Store::current_identity() const
{
    if (!fs::exists(root_ / "current"))
        return {};
    auto identity = read_text(root_, root_ / "current", 65);
    if (identity.ends_with('\n'))
        identity.pop_back();
    return identity;
}

Version Store::current() const
{
    const auto identity = current_identity();
    return identity.empty() ? Version{{}, provider_, {}, 0, {}} : version(identity);
}

VersionHead Store::head(std::string_view identity) const
{
    require_identity(identity);
    const auto path = root_ / "versions" / (std::string(identity) + ".json");
    const auto doc = Json::parse(read_text(root_, path, manifest_limit));
    if (doc.at("format") != 2 || doc.at("provider") != provider_)
        throw std::runtime_error(
            "dataset manifest has a different format or provider: " + path.string());
    VersionHead result{std::string(identity), {}, doc.at("created_ns").get<std::int64_t>(), {}};
    if (!doc.at("parent").is_null())
        result.parent = doc.at("parent").get<std::string>();
    for (const auto& [name, digest] : doc.at("shards").items())
        result.shards.emplace(name, digest.get<std::string>());
    return result;
}

std::map<std::string, Partition>
Store::shard(const std::string& name, const std::string& digest) const
{
    require_identity(digest);
    const auto content = read_text(root_, root_ / "shards" / (digest + ".json"), manifest_limit);
    if (sha256_bytes(content) != digest)
        throw std::runtime_error("dataset shard differs from its name: " + digest);
    const auto partitions = Json::parse(content);
    std::map<std::string, Partition> result;
    for (const auto& [key, item] : partitions.items()) {
        if (shard_of(key) != name)
            throw std::runtime_error("dataset shard holds another shard's partition");
        result.emplace(
            key,
            Partition{
                item.at("file").get<std::string>(),
                item.at("sha256").get<std::string>(),
                item.at("rows").get<std::uint64_t>(),
                item.value("days", std::vector<std::string>{}),
                item.value("source", std::string{})
            });
    }
    return result;
}

Version Store::version(std::string_view identity) const
{
    const auto top = head(identity);
    Version result{top.identity, provider_, top.parent, top.created_ns, {}};
    for (const auto& [name, digest] : top.shards)
        result.partitions.merge(shard(name, digest));
    return result;
}

Change Store::compare(const VersionHead& version, const VersionHead& base) const
{
    const auto load = [&](const VersionHead& side, const std::string& name) {
        const auto found = side.shards.find(name);
        return found == side.shards.end() ? std::map<std::string, Partition>{}
                                          : shard(name, found->second);
    };
    std::set<std::string> changed;
    for (const auto& [name, digest] : version.shards)
        if (!base.shards.contains(name) || base.shards.at(name) != digest)
            changed.insert(name);
    for (const auto& [name, digest] : base.shards)
        if (!version.shards.contains(name))
            changed.insert(name);
    Change change;
    for (const auto& name : changed) {
        const auto now = load(version, name), before = load(base, name);
        for (const auto& [key, part] : now) {
            const auto old = before.find(key);
            if (old == before.end())
                ++change.added;
            else if (old->second.sha256 != part.sha256)
                ++change.replaced;
        }
        for (const auto& [key, part] : before)
            change.removed += !now.contains(key);
    }
    return change;
}

fs::path Store::path_of(const Partition& part) const
{
    const fs::path relative(part.file);
    if (relative.is_absolute() || relative.empty() || *relative.begin() != "files")
        throw std::runtime_error("invalid partition path in dataset manifest");
    const auto path = contained(root_, root_ / relative);
    if (!fs::is_regular_file(path))
        throw Error(ErrorCode::not_found, "dataset partition file is missing");
    return path;
}

fs::path Store::file(const Version& version, std::string_view key) const
{
    require_key(key);
    const auto part = version.partitions.find(std::string(key));
    if (part == version.partitions.end())
        throw Error(ErrorCode::not_found, "partition is absent from this dataset version");
    return path_of(part->second);
}

std::pair<Partition, fs::path> Store::locate(std::string_view identity, std::string_view key) const
{
    require_key(key);
    const auto top = head(identity);
    const auto name = shard_of(std::string(key));
    if (const auto digest = top.shards.find(name); digest != top.shards.end()) {
        const auto parts = shard(name, digest->second);
        if (const auto part = parts.find(std::string(key)); part != parts.end())
            return {part->second, path_of(part->second)};
    }
    throw Error(ErrorCode::not_found, "partition is absent from this dataset version");
}

Writer Store::writer()
{
    return Writer(*this);
}

std::vector<Row> Store::read(const Version& version, std::string_view key) const
{
    const auto path = file(version, key);
    const auto schema = columns(key.substr(0, key.find('/')));
    parquet::Engine engine;
    auto result = engine.run("SELECT * FROM read_parquet(" + parquet::literal(path) + ")");
    if (result->ColumnCount() != schema.size())
        throw std::runtime_error("warehouse partition has a different schema");
    std::vector<Row> rows;
    rows.reserve(result->RowCount());
    for (std::size_t r = 0; r < result->RowCount(); ++r) {
        Row row;
        for (std::size_t c = 0; c < schema.size(); ++c) {
            const auto cell = result->GetValue(c, r);
            if (cell.IsNull())
                row.emplace_back();
            else if (schema[c].kind == Kind::boolean)
                row.emplace_back(cell.GetValue<bool>());
            else if (schema[c].kind == Kind::integer)
                row.emplace_back(cell.GetValue<std::int64_t>());
            else
                row.emplace_back(cell.ToString());
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

Writer::Writer(Store& store)
    : store_(store),
      lock_(store.writes_),
      base_(store.current()),
      pending_(base_)
{}

void Writer::put(std::string key, std::span<const Row> rows, std::vector<std::string> days)
{
    require_key(key);
    const auto schema = columns(std::string_view(key).substr(0, key.find('/')));
    for (const auto& day : days)
        require_day(day);
    std::sort(days.begin(), days.end());
    days.erase(std::unique(days.begin(), days.end()), days.end());
    const auto relative = fs::path("files") / key;
    const auto directory = store_.root_ / relative.parent_path();
    create_directories_durably(directory);

    struct Temporary
    {
        fs::path file;

        ~Temporary()
        {
            std::error_code ignored;
            fs::remove(file, ignored);
            fs::remove(file.string() + ".tmp", ignored);
        }
    } temporary{directory / (".partition-" + unique_process_id() + ".parquet")};

    {
        parquet::Engine engine;
        std::string definition;
        for (const auto& column : schema)
            definition += (definition.empty() ? "" : ", ") + std::string("\"")
                + std::string(column.name) + "\" " + logical(column.kind).ToString();
        engine.run("CREATE TEMP TABLE cells(" + definition + ")");
        duckdb::Appender appender(*engine.connection, "cells");
        for (const auto& row : rows) {
            if (row.size() != schema.size())
                throw std::invalid_argument("warehouse row has the wrong column count");
            appender.BeginRow();
            for (std::size_t c = 0; c < schema.size(); ++c)
                appender.Append(value(schema[c], row[c]));
            appender.EndRow();
        }
        appender.Close();
        parquet::publish(engine, "cells", temporary.file);
    }
    const auto digest = sha256_file(temporary.file);
    const auto file = relative.parent_path()
        / (relative.filename().string() + "-" + digest.substr(0, 16) + ".parquet");
    if (!fs::exists(store_.root_ / file))
        publish_file_durably(temporary.file, store_.root_ / file);
    pending_
        .partitions[std::move(key)] = {file.generic_string(), digest, rows.size(), std::move(days)};
}

void Writer::adopt(const std::string& key, const Store& from, const Version& version)
{
    const auto& part = version.partitions.at(key);
    // Files are named by their digest, so an existing name holds the same bytes.
    const auto target = store_.root_ / part.file;
    if (!fs::exists(target)) {
        create_directories_durably(target.parent_path());
        fs::create_hard_link(from.file(version, key), target);
        sync_directory(target.parent_path());
    }
    pending_.partitions[key] = {part.file, part.sha256, part.rows, part.days, from.provider()};
}

void Writer::clear()
{
    pending_.partitions.clear();
}

Version Writer::commit()
{
    if (pending_.partitions == base_.partitions)
        return base_;
    pending_.parent = base_.identity;
    pending_.created_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                              .count();
    std::map<std::string, Json> shards;
    for (const auto& [key, part] : pending_.partitions)
        shards[shard_of(key)][key] = entry(part);
    Json names = Json::object();
    create_directories_durably(store_.root_ / "shards");
    for (const auto& [name, partitions] : shards) {
        const auto content = partitions.dump();
        if (content.size() > manifest_limit)
            throw Error(ErrorCode::resource_exhausted, "dataset shard is too large: " + name);
        const auto digest = sha256_bytes(content);
        // Named by content: an existing shard holds the same bytes.
        if (!fs::exists(store_.root_ / "shards" / (digest + ".json")))
            replace_file_durably(store_.root_ / "shards" / (digest + ".json"), content);
        names[name] = digest;
    }
    const auto content = Json{
        {"format", 2},
        {"provider", pending_.provider},
        {"parent", pending_.parent.empty() ? Json(nullptr) : Json(pending_.parent)},
        {"created_ns", pending_.created_ns},
        {"shards", std::move(names)}
    }.dump();
    pending_.identity = sha256_bytes(content);
    create_directories_durably(store_.root_ / "versions");
    replace_file_durably(store_.root_ / "versions" / (pending_.identity + ".json"), content);
    replace_file_durably(store_.root_ / "current", pending_.identity);
    base_ = pending_;
    return base_;
}

} // namespace asterion::history::warehouse
