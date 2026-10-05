#pragma once
#include <asterion/kernel/plugin.hpp>
#include <asterion/domain/risk_port.hpp>
#include <asterion/kernel/native_plugin.hpp>
#include <asterion/plugin/risk.h>
#include <utility>
namespace asterion {
// The runtime owns the library through every instance; no provider-owned C++ type crosses ABI.
class NativeRisk final : public RiskPort, public Plugin {
public:
  NativeRisk(NativeLibrary library,
             const std::vector<std::pair<std::string, std::string>>& settings);
  PluginDescriptor descriptor() const override;
  void start() override;
  void stop() noexcept override;
  RiskDecision evaluate(const PreTradeRiskContext&) const override;
  const std::string& artifact() const { return library_.sha256(); }

private:
  NativeLibrary library_;
  std::unique_ptr<NativeInstance> instance_;
  const AstRiskV1* table_ = nullptr;
  bool running_ = false;
};
} // namespace asterion
