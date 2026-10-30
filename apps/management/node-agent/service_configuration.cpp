#include "service_configuration.hpp"
#include "managed_paths.hpp"
#include <asterion/foundation/serialization.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <fstream>

namespace asterion::agent {
namespace fs = std::filesystem;
namespace wire = node::proto;

namespace {

Json configuration_json(const ServiceConfiguration& configuration)
{
    Json value{
        {"kind", static_cast<int>(configuration.kind)},
        {"provider_artifact", configuration.provider_artifact},
        {"artifact", configuration.artifact},
        {"port", configuration.port},
        {"desired", configuration.desired},
        {"directory", configuration.directory}
    };
    value["plugin_artifacts"] = configuration.plugin_artifacts;
    if (!configuration.catalog_artifact.empty())
        value["catalog_artifact"] = configuration.catalog_artifact;
    if (!configuration.python.empty())
        value["python"] = configuration.python;
    return value;
}

} // namespace

std::string program_name(node::proto::ServiceKind kind)
{
    switch (kind) {
        case node::proto::MARKET:
            return "market";
        case node::proto::DATA:
            return "data";
        case node::proto::RESEARCH:
            return "research";
        case node::proto::LAB:
            return "lab";
        default:
            throw std::invalid_argument("unknown service kind");
    }
}

std::string executable_name(node::proto::ServiceKind kind)
{
    if (kind == node::proto::LAB)
        throw std::invalid_argument("the lab service runs the bundled Python");
    return "asterion_" + program_name(kind);
}

std::string service_revision(const ServiceConfiguration& configuration)
{
    return sha256_bytes(configuration_json(configuration).dump());
}

void save_service_configuration(
    const fs::path& folder,
    const ServiceConfiguration& configuration_value)
{
    const auto path = folder / "service.json";
    const auto pending = path.parent_path() / "service.pending";
    require_managed_path(path);
    require_managed_path(pending);
    if (fs::exists(pending))
        throw std::runtime_error("unfinished service configuration requires explicit recovery");
    const auto configuration = configuration_json(configuration_value);
    write_file_durably(pending, configuration.dump());
    fs::rename(pending, path);
    sync_directory(path.parent_path());
}

namespace {

ServiceConfiguration
read_service_configuration(const fs::path& folder, bool local, unsigned short control_port)
{
    const auto path = folder / "service.json";
    require_managed_path(path);
    require_managed_path(folder / "service.pending");
    if (fs::exists(folder / "service.pending"))
        throw std::runtime_error("unfinished service configuration requires explicit recovery");
    if (!fs::is_regular_file(path) || fs::file_size(path) > 65536)
        throw std::invalid_argument("invalid managed configuration file");
    std::ifstream input(path);
    std::string raw{std::istreambuf_iterator<char>(input), {}};
    auto document = parse_json(raw, 65536);
    if (document.size()
            != 7U + (document.contains("catalog_artifact") ? 1U : 0U)
                + (document.contains("python") ? 1U : 0U)
        || !document.at("desired").is_boolean() || !document.at("port").is_number_unsigned())
        throw std::invalid_argument("invalid managed service configuration");
    ServiceConfiguration configuration;
    configuration.kind = static_cast<wire::ServiceKind>(document.at("kind").get<int>());
    configuration.provider_artifact = document.at("provider_artifact").get<std::string>();
    configuration.plugin_artifacts =
        document.at("plugin_artifacts").get<std::vector<std::string>>();
    if (document.contains("catalog_artifact")) {
        if (configuration.kind != wire::MARKET)
            throw std::invalid_argument("CTP catalog library only belongs to market data");
        configuration.catalog_artifact = document.at("catalog_artifact").get<std::string>();
        validate_artifact_digest(configuration.catalog_artifact);
    }
    if (configuration.kind != wire::MARKET && configuration.kind != wire::RESEARCH
        && configuration.kind != wire::DATA && configuration.kind != wire::LAB)
        throw std::invalid_argument("unsupported service kind");
    if (document.contains("python")) {
        configuration.python = document.at("python").get<std::string>();
        const fs::path interpreter(
            std::u8string(configuration.python.begin(), configuration.python.end()));
        if (configuration.kind != wire::LAB || !interpreter.is_absolute())
            throw std::invalid_argument("only the lab service names a Python interpreter");
    }
    if (configuration.kind == wire::LAB
        && (configuration.python.empty() || !local || !configuration.plugin_artifacts.empty()))
        throw std::invalid_argument("the lab service runs the bundled Python on a local node");
    if (!configuration.provider_artifact.empty()) {
        if (configuration.kind != wire::MARKET)
            throw std::invalid_argument("provider library only belongs to market data");
        validate_artifact_digest(configuration.provider_artifact);
    }
    if (configuration.kind == wire::RESEARCH) {
        if (!configuration.provider_artifact.empty())
            throw std::invalid_argument("research service does not load a market provider");
    }
    configuration.artifact = document.at("artifact").get<std::string>();
    validate_artifact_digest(configuration.artifact);
    const auto port = document.at("port").get<unsigned int>();
    if (port > 65535 || (local ? port != 0 : (!port || port == control_port)))
        throw std::invalid_argument("invalid managed port");
    configuration.port = static_cast<unsigned short>(port);
    configuration.desired = document.at("desired").get<bool>();
    configuration.directory = document.at("directory").get<std::string>();
    const fs::path ledger(
        std::u8string(configuration.directory.begin(), configuration.directory.end()));
    if (!ledger.is_absolute())
        throw std::invalid_argument("ledger must be absolute");
    require_managed_path(ledger);
    if (!fs::is_directory(ledger))
        throw std::invalid_argument("managed ledger is missing");
    return configuration;
}

} // namespace

ServiceConfiguration
load_service_configuration(const fs::path& folder, bool local, unsigned short control_port)
{
    // Every refusal names the file, so a node left by an older build can be found.
    const auto file = (folder / "service.json").string();
    try {
        return read_service_configuration(folder, local, control_port);
    } catch (const std::runtime_error& error) {
        throw std::runtime_error(file + ": " + error.what());
    } catch (const std::exception& error) {
        throw std::invalid_argument(file + ": " + error.what());
    }
}

} // namespace asterion::agent
