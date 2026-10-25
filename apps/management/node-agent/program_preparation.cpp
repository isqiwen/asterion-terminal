#include "program_preparation.hpp"
#include "managed_paths.hpp"
#include "plugin_artifacts.hpp"
#include "service_configuration.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/protocol/trading.hpp>
#include <fstream>
#include <map>
#include <iostream>
#include <set>

namespace asterion::agent {
namespace fs = std::filesystem;

void prepare_service_programs(
    const fs::path& root,
    const fs::path& manifest,
    bool local,
    unsigned short port)
{
    if (!root.is_absolute() || !fs::is_directory(root))
        throw std::invalid_argument("agent requires an existing absolute directory");
    require_managed_path(root);
    FileLock stopped(root, "agent.lock");
    require_managed_path(manifest);
    if (fs::file_size(manifest) > 1024 * 1024)
        throw std::invalid_argument("service program manifest is too large");
    node::proto::ServicePrograms sources;
    std::ifstream input(manifest, std::ios::binary);
    if (!sources.ParseFromIstream(&input) || sources.version() != 1)
        throw std::invalid_argument("unsupported service program manifest");
    protocol::validate_message(sources);
    create_directories_durably(root / "artifacts");
    create_directories_durably(root / "services");
    std::map<std::string, ServiceConfiguration> configurations;
    for (const auto& entry : fs::directory_iterator(root / "services")) {
        validate_service_id(entry.path().filename().string());
        log_process_event(
            "deployment",
            LogLevel::info,
            "program.read_configuration",
            {{"file", (entry.path() / "service.json").string()}});
        configurations.emplace(
            entry.path().filename().string(),
            load_service_configuration(entry.path(), local, port));
    }
    // Validate and stage the complete selection before changing any service reference.
    // A failed preparation never starts a child; a later run can repeat it safely.
    const auto stage = root / ("program-stage-" + unique_process_id());
    create_directories_durably(stage);

    struct Cleanup
    {
        fs::path path;

        ~Cleanup()
        {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } cleanup{stage};

    std::map<std::string, std::string> staged;
    auto install = [&](const std::string& name) -> std::string {
        if (name.empty())
            return {};
        if (const auto found = staged.find(name); found != staged.end())
            return found->second;
        const fs::path source(name);
        if (!source.is_absolute() || !fs::is_regular_file(source) || fs::is_symlink(source))
            throw std::invalid_argument("invalid service program source");
        const auto target = artifact_platform(source), host = current_platform();
        if (target.os != host.os || target.arch != host.arch)
            throw std::invalid_argument("service program platform mismatch");
        const auto hash = sha256_file(source);
        const auto destination = root / "artifacts" / (hash + ".bin");
        require_managed_path(destination);
        if (fs::exists(destination) && sha256_file(destination) != hash)
            throw std::invalid_argument("service program artifact is corrupted");
        if (!fs::exists(destination)) {
            const auto temporary = stage / hash;
            fs::copy_file(source, temporary);
            if (sha256_file(temporary) != hash)
                throw std::invalid_argument("service program changed during preparation");
            publish_file_durably(temporary, destination);
        }
        staged.emplace(name, hash);
        return hash;
    };
    // Only the programs of a kind of service this node has are staged: a
    // service deployed later brings its own, and a copy nothing runs would be
    // left behind by every build.
    std::set<node::proto::ServiceKind> present, listed;
    for (const auto& [name, config] : configurations)
        present.insert(config.kind);
    std::map<node::proto::ServiceKind, ServiceConfiguration> programs;
    for (const auto& source : sources.services()) {
        const bool market = source.kind() == node::proto::MARKET;
        const bool trading = source.kind() == node::proto::TRADING;
        const bool task = source.kind() == node::proto::RESEARCH;
        if ((!market && !trading && !task && source.kind() != node::proto::DATA)
            || source.executable().empty() || source.provider().empty() == market
            || source.catalog().empty() == (market || trading)

            || !listed.insert(source.kind()).second)
            throw std::invalid_argument("invalid development service program selection");
        if (!present.contains(source.kind()))
            continue;
        ServiceConfiguration p;
        p.kind = source.kind();
        p.artifact = install(source.executable());
        p.provider_artifact = install(source.provider());
        p.catalog_artifact = install(source.catalog());
        programs.emplace(p.kind, std::move(p));
    }
    PluginArtifacts plugins(root, current_executable());
    std::map<std::string, std::string> bundled;
    for (const auto& source : sources.bundled_plugins()) {
        const auto hash = install(source);
        if (!bundled.emplace(plugins.identity(hash), hash).second)
            throw std::invalid_argument("duplicate bundled plugin identity");
    }
    std::map<std::string, ServiceConfiguration> changed;
    for (auto& [name, config] : configurations) {
        auto next = programs.at(config.kind);
        next.directory = config.directory;
        next.port = config.port;
        next.desired = config.desired;
        const auto identities = plugins.verify(config.plugin_artifacts);
        next.plugin_artifacts = config.plugin_artifacts;
        for (auto& hash : next.plugin_artifacts) {
            const auto found = bundled.find(identities.at(hash));
            if (found != bundled.end())
                hash = found->second;
        }
        if (next != config)
            changed.emplace(name, std::move(next));
    }
    for (const auto& [name, config] : changed)
        save_service_configuration(root / "services" / name, config);
    // A service program is built again for every change, and nothing but a
    // service's configuration names its copy here: one that a configuration
    // named and none names any more is deleted, or every build would leave its
    // programs behind. What a service lists as its plugins stays, because a task
    // is launched with the plugins it was submitted under; the folders they are
    // loaded from are copies of them.
    std::set<std::string> kept;
    for (const auto& [name, config] : configurations) {
        const auto found = changed.find(name);
        const auto& now = found == changed.end() ? config : found->second;
        kept.insert({now.artifact, now.provider_artifact, now.catalog_artifact});
        kept.insert(now.plugin_artifacts.begin(), now.plugin_artifacts.end());
        kept.insert(config.plugin_artifacts.begin(), config.plugin_artifacts.end());
    }
    std::size_t superseded = 0;
    for (const auto& [name, config] : configurations) {
        for (const auto& hash :
            {config.artifact, config.provider_artifact, config.catalog_artifact}) {
            const auto copy = root / "artifacts" / (hash + ".bin");
            if (hash.empty() || kept.contains(hash))
                continue;
            require_managed_path(copy);
            superseded += fs::remove(copy);
        }
        plugins.discard(name);
    }
    if (superseded)
        sync_directory(root / "artifacts");
    log_process_event(
        "deployment",
        LogLevel::info,
        "program.synchronized",
        {{"services", changed.size()}, {"superseded_programs", superseded}});
}

} // namespace asterion::agent
