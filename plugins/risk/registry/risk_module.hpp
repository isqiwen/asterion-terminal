#pragma once
#include "native_risk.hpp"
#include "order_limits.hpp"
namespace asterion::risk_providers {
// A module selection is immutable. A captured copy belongs to its ledger/task.
class Module {
public:
  static Module selected();
  static Module pinned(const std::filesystem::path& directory, const std::string& artifact);
  static std::filesystem::path filename(const std::filesystem::path& directory);
  const std::string& artifact() const { return library_.sha256(); }
  void capture(const std::filesystem::path& directory) const;
  std::shared_ptr<NativeRisk> create(const OrderLimitsConfig& config) const;

private:
  explicit Module(NativeLibrary library) : library_(std::move(library)) {}
  NativeLibrary library_;
};
} // namespace asterion::risk_providers
