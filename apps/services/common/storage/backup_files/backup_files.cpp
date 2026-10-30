#include "backup_files.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <set>
#include <tuple>

namespace asterion::backup_files {
namespace fs = std::filesystem;

namespace {

constexpr std::size_t max_files = 100000, max_manifest_bytes = 32 * 1024 * 1024;

void cancelled(std::stop_token stop)
{
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "backup file operation interrupted");
}

void digest(const std::string& value)
{
    if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid backup content digest");
}

void relative_path(const fs::path& value)
{
    if (value.empty() || value.is_absolute() || value != value.lexically_normal()
        || value.generic_string().size() > 1024
        || value.generic_string().find('\\') != std::string::npos)
        throw std::invalid_argument("invalid backup relative path");
    for (const auto& part : value)
        if (part == "." || part == ".." || part.empty())
            throw std::invalid_argument("invalid backup relative path");
}

Json owner_json(const resource::proto::Owner& owner)
{
    validate_id(owner.node_id());
    validate_id(owner.service_id());
    if (owner.domain() != "data" && owner.domain() != "research" && owner.domain() != "market"
        && owner.domain() != "intelligence")
        throw std::invalid_argument("invalid backup owner domain");
    return {{"node", owner.node_id()}, {"service", owner.service_id()}, {"domain", owner.domain()}};
}

void validate_owner(const Json& value)
{
    require_fields(value, {"node", "service", "domain"});
    resource::proto::Owner owner;
    owner.set_node_id(value.at("node").get<std::string>());
    owner.set_service_id(value.at("service").get<std::string>());
    owner.set_domain(value.at("domain").get<std::string>());
    (void)owner_json(owner);
}

Json object_json(const resource::proto::Object& value)
{
    validate_id(value.kind());
    validate_id(value.id());
    if (value.version().empty() || value.version().size() > 256)
        throw std::invalid_argument("invalid backup dependency version");
    return {
        {"owner", owner_json(value.owner())},
        {"kind", value.kind()},
        {"id", value.id()},
        {"version", value.version()}
    };
}

std::string read_bytes(const fs::path& path, std::size_t maximum)
{
    if (fs::is_symlink(path) || !fs::is_regular_file(path) || fs::file_size(path) > maximum)
        throw std::invalid_argument("backup metadata missing or unsupported");
    std::ifstream input(path, std::ios::binary);
    std::string bytes(maximum + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    if (input.bad() || !input.eof() || bytes.size() > maximum)
        throw std::runtime_error("backup metadata read failed");
    return bytes;
}

fs::path owned_root(const fs::path& path)
{
    if (!path.is_absolute() || fs::is_symlink(path) || !fs::is_directory(path))
        throw std::invalid_argument("backup requires an existing absolute owned directory");
    return fs::canonical(path);
}

bool within(const fs::path& value, const fs::path& parent)
{
    const auto relative = value.lexically_relative(parent);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

fs::path new_target(const fs::path& path)
{
    if (!path.is_absolute() || path.filename().empty() || path.filename() == "."
        || path.filename() == "..")
        throw std::invalid_argument("choose a new absolute backup directory");
    return owned_root(path.parent_path()) / path.filename();
}

fs::path payload(const fs::path& root, const Json& file)
{
    const fs::path path(file.at("path").get<std::string>());
    relative_path(path);
    return root / "owners" / std::to_string(file.at("owner").get<std::size_t>()) / path;
}

void validate_plan(const Json& plan)
{
    if (!plan.is_object() || plan.size() != 7U + (plan.contains("scopes") ? 1U : 0U))
        throw Error(ErrorCode::invalid_request, "unexpected object fields");
    for (const auto* field :
        {"backup_id",
            "owners",
            "files",
            "directories",
            "dependencies",
            "exclusions",
            "limitations"})
        if (!plan.contains(field))
            throw Error(ErrorCode::invalid_request, "missing object field");
    validate_id(plan.at("backup_id").get<std::string>());
    const auto& owners = plan.at("owners");
    if (!owners.is_array() || owners.empty() || owners.size() > 32)
        throw std::invalid_argument("invalid backup owner count");
    std::set<std::string> identities;
    for (const auto& owner : owners) {
        validate_owner(owner);
        if (!identities.insert(owner.dump()).second)
            throw std::invalid_argument("duplicate backup owner");
    }
    if (plan.contains("scopes")) {
        const auto& scopes = plan.at("scopes");
        if (!scopes.is_array() || scopes.size() > 512)
            throw std::invalid_argument("invalid backup business scope");
        std::set<std::pair<std::size_t, std::string>> fields;
        for (const auto& scope : scopes) {
            require_fields(scope, {"owner", "key", "value"});
            if (!scope.at("owner").is_number_unsigned()
                || scope.at("owner").get<std::size_t>() >= owners.size()
                || !scope.at("key").is_string() || !scope.at("value").is_string())
                throw std::invalid_argument("invalid backup business scope");
            const auto key = scope.at("key").get<std::string>();
            const auto value = scope.at("value").get<std::string>();
            validate_id(key);
            if (value.empty() || value.size() > 4096 || value.find('\0') != std::string::npos
                || !fields.emplace(scope.at("owner").get<std::size_t>(), key).second)
                throw std::invalid_argument("invalid backup business scope");
        }
    }
    const auto& files = plan.at("files");
    if (!files.is_array() || files.empty() || files.size() > max_files)
        throw std::invalid_argument("invalid backup file count");
    std::set<std::pair<std::size_t, std::string>> paths;
    for (const auto& file : files) {
        require_fields(file, {"owner", "path", "format", "bytes", "sha256"});
        if (!file.at("owner").is_number_unsigned()
            || file.at("owner").get<std::size_t>() >= owners.size()
            || !file.at("bytes").is_number_unsigned())
            throw std::invalid_argument("invalid backup file metadata");
        const fs::path path(file.at("path").get<std::string>());
        relative_path(path);
        const auto format = file.at("format").get<std::string>();
        if (format != "sqlite" && format != "protobuf" && format != "json" && format != "parquet"
            && format != "binary" && format != "coordination")
            throw std::invalid_argument("unsupported backup file format");
        digest(file.at("sha256").get<std::string>());
        if (format == "coordination"
            && (file.at("bytes").get<std::uint64_t>() != 0
                || file.at("sha256").get<std::string>() != sha256_bytes("")))
            throw std::invalid_argument("backup coordination slots must be empty");
        if (!paths.emplace(file.at("owner").get<std::size_t>(), path.generic_string()).second)
            throw std::invalid_argument("duplicate backup file");
    }
    if (!plan.at("directories").is_array() || plan.at("directories").size() > max_files)
        throw std::invalid_argument("invalid backup directory count");
    std::set<std::pair<std::size_t, std::string>> directories;
    for (const auto& directory : plan.at("directories")) {
        require_fields(directory, {"owner", "path"});
        if (!directory.at("owner").is_number_unsigned()
            || directory.at("owner").get<std::size_t>() >= owners.size())
            throw std::invalid_argument("invalid backup directory owner");
        const fs::path path(directory.at("path").get<std::string>());
        relative_path(path);
        const auto identity =
            std::pair{directory.at("owner").get<std::size_t>(), path.generic_string()};
        if (!directories.insert(identity).second || paths.contains(identity))
            throw std::invalid_argument("duplicate or conflicting backup directory");
    }
    if (!plan.at("dependencies").is_array() || plan.at("dependencies").size() > max_files
        || !plan.at("exclusions").is_array() || plan.at("exclusions").size() > max_files)
        throw std::invalid_argument("invalid backup scope evidence");
    for (const auto& dependency : plan.at("dependencies")) {
        require_fields(dependency, {"object", "included"});
        const auto& object = dependency.at("object");
        require_fields(object, {"owner", "kind", "id", "version"});
        validate_owner(object.at("owner"));
        validate_id(object.at("kind").get<std::string>());
        validate_id(object.at("id").get<std::string>());
        const auto version = object.at("version").get<std::string>();
        if (version.empty() || version.size() > 256 || !dependency.at("included").is_boolean())
            throw std::invalid_argument("invalid backup dependency");
        if (dependency.at("included").get<bool>()
            && !identities.contains(object.at("owner").dump()))
            throw std::invalid_argument("included backup dependency has no selected owner");
    }
    if (!plan.at("limitations").is_array() || plan.at("limitations").size() > max_files)
        throw std::invalid_argument("invalid backup limitations");
    for (const auto& limitation : plan.at("limitations")) {
        require_fields(limitation, {"owner", "reason"});
        if (!limitation.at("owner").is_number_unsigned()
            || limitation.at("owner").get<std::size_t>() >= owners.size()
            || !limitation.at("reason").is_string()
            || limitation.at("reason").get<std::string>().size() > 256)
            throw std::invalid_argument("invalid backup limitation");
    }
    for (const auto& exclusion : plan.at("exclusions")) {
        require_fields(exclusion, {"owner", "category"});
        if (!exclusion.at("owner").is_number_unsigned()
            || exclusion.at("owner").get<std::size_t>() >= owners.size()
            || !exclusion.at("category").is_string()
            || exclusion.at("category").get<std::string>().size() > 256)
            throw std::invalid_argument("invalid backup exclusion");
    }
}

void verify_file(const fs::path& path, const Json& file)
{
    if (fs::is_symlink(path) || !fs::is_regular_file(path) || fs::hard_link_count(path) != 1
        || fs::canonical(path) != path.lexically_normal()
        || fs::file_size(path) != file.at("bytes").get<std::uint64_t>()
        || sha256_file(path) != file.at("sha256").get<std::string>())
        throw std::invalid_argument("backup payload missing or content check failed");
}

void copy_file(
    const fs::path& source,
    const fs::path& destination,
    const Json& file,
    const std::string& operation,
    std::stop_token stop)
{
    cancelled(stop);
    if (fs::exists(destination)) {
        verify_file(destination, file);
        return;
    }
    create_directories_durably(destination.parent_path());
    const auto temporary = destination.parent_path()
        / (".copying-" + operation + "-" + file.at("sha256").get<std::string>());
    if (fs::is_symlink(temporary)
        || (fs::exists(temporary)
            && (!fs::is_regular_file(temporary) || fs::hard_link_count(temporary) != 1)))
        throw std::invalid_argument("invalid interrupted backup copy");
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!input || !output)
        throw std::runtime_error("backup file copy cannot open payload");
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write);
    std::array<char, 256 * 1024> bytes{};
    while (input) {
        cancelled(stop);
        input.read(bytes.data(), bytes.size());
        output.write(bytes.data(), input.gcount());
        if (!output)
            throw std::runtime_error("backup file copy failed");
    }
    if (!input.eof())
        throw std::runtime_error("backup source read failed");
    output.close();
    if (!output)
        throw std::runtime_error("backup file close failed");
    verify_file(temporary, file);
    publish_file_durably(temporary, destination);
}

void create_payload_directories(const fs::path& root, const Json& plan)
{
    for (const auto& directory : plan.at("directories"))
        create_directories_durably(payload(root, directory));
}

void verify_contents(const fs::path& root, const Json& plan, std::stop_token stop)
{
    if (fs::file_size(root / "backup.lock") != 0)
        throw std::invalid_argument("backup lock slot contains unexpected content");
    std::set<fs::path> expected{
        root / "backup.lock",
        root / "plan.json",
        root / "manifest.json",
        root / "complete.sha256"
    };
    for (const auto& directory : plan.at("directories")) {
        const auto path = payload(root, directory);
        if (fs::is_symlink(path) || !fs::is_directory(path)
            || fs::canonical(path) != path.lexically_normal())
            throw std::invalid_argument("backup declared directory missing");
    }
    for (const auto& file : plan.at("files")) {
        cancelled(stop);
        const auto path = payload(root, file);
        verify_file(path, file);
        expected.insert(path);
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        cancelled(stop);
        if (entry.is_symlink()
            || (!entry.is_directory()
                && (!entry.is_regular_file() || !expected.contains(entry.path()))))
            throw std::invalid_argument("backup contains undeclared files");
    }
}

Json inspect_locked(const fs::path& root, std::stop_token stop)
{
    const auto bytes = read_bytes(root / "manifest.json", max_manifest_bytes);
    if (read_bytes(root / "complete.sha256", 64) != sha256_bytes(bytes))
        throw std::invalid_argument("backup manifest check failed or publication incomplete");
    auto plan = parse_json(bytes);
    validate_plan(plan);
    if (parse_json(read_bytes(root / "plan.json", max_manifest_bytes)) != plan)
        throw std::invalid_argument("backup preparation and publication differ");
    verify_contents(root, plan, stop);
    return plan;
}

void publish_manifest(const fs::path& root, const Json& plan)
{
    const auto bytes = plan.dump();
    replace_file_durably(root / "manifest.json", bytes);
    replace_file_durably(root / "complete.sha256", sha256_bytes(bytes));
}

} // namespace

Cut::Cut(std::vector<Snapshot> snapshots)
    : snapshots_(std::move(snapshots))
{
    if (snapshots_.empty() || snapshots_.size() > 32)
        throw std::invalid_argument("select a supported backup scope");
    std::ranges::sort(snapshots_, {}, [](const Snapshot& value) {
        return owner_json(value.owner).dump();
    });
    for (auto& snapshot : snapshots_) {
        if (!snapshot.ownership || owned_root(snapshot.directory) != snapshot.directory)
            throw std::invalid_argument("backup requires the domain cold ownership cut");
        std::ranges::sort(snapshot.files, {}, [](const File& value) {
            return value.relative.generic_string();
        });
        for (const auto& file : snapshot.files) {
            relative_path(file.relative);
            if (!file.source.is_absolute() || fs::is_symlink(file.source)
                || !fs::is_regular_file(file.source)
                || fs::canonical(file.source) != file.source.lexically_normal())
                throw std::invalid_argument("invalid domain-selected backup source");
        }
    }
}

Json Cut::preview(const std::string& operation_id, std::stop_token stop) const
{
    validate_id(operation_id);
    Json plan{
        {"backup_id", operation_id},
        {"owners", Json::array()},
        {"files", Json::array()},
        {"directories", Json::array()},
        {"dependencies", Json::array()},
        {"exclusions", Json::array()},
        {"limitations", Json::array()}
    };
    for (std::size_t i = 0; i < snapshots_.size(); ++i) {
        const auto& snapshot = snapshots_[i];
        plan["owners"].push_back(owner_json(snapshot.owner));
        if (!snapshot.scope.empty()) {
            if (!plan.contains("scopes"))
                plan["scopes"] = Json::array();
            for (const auto& [key, value] : snapshot.scope)
                plan["scopes"].push_back({{"owner", i}, {"key", key}, {"value", value}});
        }
        for (const auto& file : snapshot.files) {
            cancelled(stop);
            plan["files"].push_back(
                {{"owner", i},
                    {"path", file.relative.generic_string()},
                    {"format", file.format},
                    {"bytes", fs::file_size(file.source)},
                    {"sha256", sha256_file(file.source)}});
            if (plan["files"].size() > max_files)
                throw Error(ErrorCode::resource_exhausted, "backup file count exceeds capacity");
        }
        for (const auto& directory : snapshot.directories) {
            relative_path(directory);
            plan["directories"].push_back({{"owner", i}, {"path", directory.generic_string()}});
        }
        for (const auto& dependency : snapshot.dependencies)
            plan["dependencies"].push_back(
                {{"object", object_json(dependency.object)}, {"included", dependency.included}});
        for (const auto& reason : snapshot.limitations)
            plan["limitations"].push_back({{"owner", i}, {"reason", reason}});
        for (const auto& category : snapshot.exclusions)
            plan["exclusions"].push_back({{"owner", i}, {"category", category}});
    }
    validate_plan(plan);
    if (plan.dump().size() > max_manifest_bytes)
        throw Error(ErrorCode::resource_exhausted, "backup manifest exceeds capacity");
    return plan;
}

Json Cut::save(
    const std::string& operation_id,
    const fs::path& destination,
    const std::string& expected_digest,
    std::stop_token stop) const
{
    digest(expected_digest);
    const auto plan = preview(operation_id, stop);
    if (sha256_bytes(plan.dump()) != expected_digest)
        throw Error(ErrorCode::conflict, "backup scope changed; preview again");
    const auto root = new_target(destination);
    for (const auto& snapshot : snapshots_) {
        if (within(root, snapshot.directory) || within(snapshot.directory, root))
            throw std::invalid_argument("backup destination overlaps source storage");
        for (const auto& file : snapshot.files)
            if (within(file.source, root))
                throw std::invalid_argument("backup destination contains a selected source");
    }
    const bool existing = fs::exists(root);
    if (!existing) {
        if (!fs::create_directory(root))
            throw Error(ErrorCode::conflict, "backup destination already exists");
        fs::permissions(root, fs::perms::owner_all);
        sync_directory(root.parent_path());
    }
    if (owned_root(root) != root)
        throw std::invalid_argument("invalid backup destination");
    if (existing && parse_json(read_bytes(root / "plan.json", max_manifest_bytes)) != plan)
        throw Error(ErrorCode::conflict, "backup destination belongs to another operation");
    FileLock lock(
        root,
        "backup.lock",
        existing ? FileLock::Access::exclusive_existing : FileLock::Access::exclusive);
    if (existing) {
        if (parse_json(read_bytes(root / "plan.json", max_manifest_bytes)) != plan)
            throw Error(ErrorCode::conflict, "backup destination belongs to another operation");
        if (fs::exists(root / "complete.sha256"))
            return inspect_locked(root, stop);
    } else
        replace_file_durably(root / "plan.json", plan.dump());
    std::uint64_t required = 0;
    for (const auto& file : plan.at("files")) {
        if (fs::exists(payload(root, file))) {
            verify_file(payload(root, file), file);
            continue;
        }
        const auto bytes = file.at("bytes").get<std::uint64_t>();
        if (bytes > std::numeric_limits<std::uint64_t>::max() - required)
            throw Error(ErrorCode::resource_exhausted, "backup size exceeds capacity");
        required += bytes;
    }
    if (required > fs::space(root).available)
        throw Error(ErrorCode::resource_exhausted, "backup destination has insufficient space");
    create_payload_directories(root, plan);
    std::size_t index = 0;
    for (const auto& snapshot : snapshots_)
        for (const auto& source : snapshot.files) {
            const auto& file = plan.at("files").at(index++);
            copy_file(source.source, payload(root, file), file, operation_id, stop);
        }
    verify_contents(root, plan, stop);
    publish_manifest(root, plan);
    return inspect_locked(root, stop);
}

Json inspect(const fs::path& path, std::stop_token stop)
{
    const auto root = owned_root(path);
    FileLock lock(root, "backup.lock", FileLock::Access::shared_existing);
    return inspect_locked(root, stop);
}

Json inspect_preparation(const fs::path& path, std::stop_token stop)
{
    const auto root = owned_root(path);
    FileLock lock(root, "backup.lock", FileLock::Access::shared_existing);
    cancelled(stop);
    if (fs::file_size(root / "backup.lock"))
        throw std::invalid_argument("backup lock slot contains unexpected content");
    if (fs::exists(root / "complete.sha256"))
        return inspect_locked(root, stop);
    auto plan = parse_json(read_bytes(root / "plan.json", max_manifest_bytes));
    validate_plan(plan);
    return plan; // Preparation is not proof that files or domain closure are complete.
}

Json restore_files(const fs::path& backup, const fs::path& destination, std::stop_token stop)
{
    const auto source = owned_root(backup);
    FileLock source_lock(source, "backup.lock", FileLock::Access::shared_existing);
    const auto plan = inspect_locked(source, stop); // No target writes before full verification.
    const auto root = new_target(destination);
    if (within(root, source) || within(source, root) || fs::exists(root))
        throw Error(ErrorCode::conflict, "restore requires a new independent destination");
    if (!fs::create_directory(root))
        throw Error(ErrorCode::conflict, "restore destination already exists");
    fs::permissions(root, fs::perms::owner_all);
    sync_directory(root.parent_path());
    FileLock target_lock(root, "backup.lock");
    replace_file_durably(root / "plan.json", plan.dump());
    create_payload_directories(root, plan);
    for (const auto& file : plan.at("files"))
        copy_file(
            payload(source, file),
            payload(root, file),
            file,
            plan.at("backup_id").get<std::string>(),
            stop);
    verify_contents(root, plan, stop);
    publish_manifest(root, plan);
    return inspect_locked(root, stop);
}

} // namespace asterion::backup_files
