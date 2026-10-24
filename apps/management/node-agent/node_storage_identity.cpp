#include "node_storage_identity.hpp"
#include "managed_paths.hpp"
#include <asterion/foundation/serialization.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <fstream>

namespace asterion::agent {

std::string node_storage_identity(const std::filesystem::path& root)
{
    require_managed_path(root);
    const auto path = root / "node.json";
    require_managed_path(path);
    if (std::filesystem::exists(path)) {
        if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 4096)
            throw std::invalid_argument("invalid persistent node identity");
        std::ifstream stream(path, std::ios::binary);
        const std::string raw{std::istreambuf_iterator<char>(stream), {}};
        if (stream.bad())
            throw std::runtime_error("cannot read persistent node identity");
        const auto record = parse_json(raw, 4096);
        if (!record.is_object() || record.size() != 1 || !record.contains("node_id")
            || !record.at("node_id").is_string())
            throw std::invalid_argument("invalid persistent node identity");
        const auto id = record.at("node_id").get<std::string>();
        validate_id(id);
        // A prior create may have failed after writing but before its directory barrier.
        sync_file_durably(path);
        return id;
    }
    const auto services = root / "services";
    require_managed_path(services);
    if (std::filesystem::exists(services) && !std::filesystem::is_empty(services))
        throw std::invalid_argument(
            "existing node has no supported persistent identity; preserve its data");
    const auto id =
        unique_process_id(); // Random identifier, persisted once; never regenerated on restart.
    write_file_durably(path, Json{{"node_id", id}}.dump());
    return id;
}

} // namespace asterion::agent
