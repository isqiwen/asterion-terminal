#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <utility>
#include <map>

namespace asterion::agent {

class PluginArtifacts
{
public:
    // inspector is an Agent executable: plugin descriptors are read in a child
    // started with --inspect-plugin, so a faulty library cannot take the Agent
    // and every service it supervises down with it.
    PluginArtifacts(std::filesystem::path root, std::filesystem::path inspector)
        : root_(std::move(root)),
          inspector_(std::move(inspector))
    {}

    std::map<std::string, std::string> verify(const std::vector<std::string>& artifacts) const;
    std::string identity(const std::string& artifact) const;
    std::filesystem::path
    materialize(const std::string& service, const std::vector<std::string>& artifacts) const;
    // Removes every folder materialize made for a service: copies of the
    // artifacts, made again where they are needed. Only while nothing of the
    // service runs.
    void discard(const std::string& service) const;

private:
    std::filesystem::path binary(const std::string& hash) const;
    std::filesystem::path root_, inspector_;
};

// Entry point of --inspect-plugin: prints the plugin identity, or fails.
int inspect_plugin(const std::string& path, const std::string& artifact);

} // namespace asterion::agent
