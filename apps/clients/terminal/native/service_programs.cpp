#include "service_programs.hpp"
#include "remote_bundle.hpp"
#include <stdexcept>
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
namespace asterion::terminal {
namespace {
// The same role-to-program mapping is used for initial deployment and program updates.
template <typename Resolve>
ServicePrograms resolve_programs(node::v1::ServiceKind kind, Resolve resolve) {
  ServicePrograms programs;
  switch (kind) {
  case node::v1::PAPER_TRADING:
    programs.executable = resolve("ASTERION_TRADING_EXECUTABLE", "asterion-trading", false);
    break;
  case node::v1::LIVE_TRADING:
    programs.executable = resolve("ASTERION_TRADING_EXECUTABLE", "asterion-trading", false);
    programs.catalog = resolve("ASTERION_CTP_CATALOG_LIBRARY", "ctp-trader", true);
    break;
  case node::v1::MARKET_DATA:
    programs.executable = resolve("ASTERION_MARKET_EXECUTABLE", "asterion-market-data", false);
    programs.provider = resolve("ASTERION_CTP_LIBRARY", "ctp-md", true);
    programs.catalog = resolve("ASTERION_CTP_CATALOG_LIBRARY", "ctp-trader", true);
    break;
  case node::v1::TASK_SERVICE:
    programs.executable = resolve("ASTERION_TASK_EXECUTABLE", "asterion-task-service", false);
    programs.worker = resolve("ASTERION_BACKTEST_EXECUTABLE", "asterion-backtest", false);
    programs.factor = resolve("ASTERION_FACTOR_EXECUTABLE", "asterion-factor", false);
    programs.data = resolve("ASTERION_DATA_PIPELINE_EXECUTABLE", "asterion-data-pipeline", false);
    break;
  case node::v1::STRATEGY:
    programs.executable = resolve("ASTERION_STRATEGY_EXECUTABLE", "asterion-strategy", false);
    break;
  default:
    throw std::invalid_argument("unknown service kind");
  }
  return programs;
}
} // namespace
ServicePrograms local_service_programs(node::v1::ServiceKind kind) {
  const auto platform = current_platform();
  const auto root = current_executable().parent_path();
  return resolve_programs(kind, [&](const char* variable, const char* name, bool library) {
    if (const auto configured = environment_path(variable))
      return *configured;
    const auto extension = library ? (platform.os == "macos"     ? ".dylib"
                                      : platform.os == "windows" ? ".dll"
                                                                 : ".so")
                                   : (platform.os == "windows" ? ".exe" : "");
    return root / (std::string(name) + extension);
  });
}
ServicePrograms bundled_service_programs(const std::string& arch, node::v1::ServiceKind kind) {
  return resolve_programs(kind, [&](const char*, const char* name, bool library) {
    return bundled_linux_program(arch, std::string(name) + (library ? ".so" : ""));
  });
}
} // namespace asterion::terminal
