#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <utility>
namespace asterion::agent {
class PluginArtifacts {
public:
  explicit PluginArtifacts(std::filesystem::path root) : root_(std::move(root)) {}
  void verify(const std::vector<std::string>& artifacts) const;
  std::filesystem::path materialize(const std::string& service,
                                    const std::vector<std::string>& artifacts) const;

private:
  std::filesystem::path binary(const std::string& hash) const;
  std::filesystem::path root_;
};
} // namespace asterion::agent
