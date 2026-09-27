#pragma once
// Internal to the Terminal application: shared state of Application and the
// per-domain command registrations (commands_*.cpp). Not a public API.
#include "terminal_application.hpp"
#include "csv_market_data.hpp"
#include "market_client.hpp"
#include "moving_average.hpp"
#include "node_client.hpp"
#include "node_enrollment.hpp"
#include "remote_bundle.hpp"
#include "research_client.hpp"
#include "strategy_client.hpp"
#include "trading_client.hpp"
#include <asterion/domain/futures.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/runtime.hpp>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <mutex>
#include <stdexcept>
#include <map>
#include <optional>

namespace asterion::terminal {
using nlohmann::json;
using namespace asterion;
void fields(const json& object, std::initializer_list<std::string_view> names);
json risk_parameters(const json& p);
std::string text(const json& object, const char* name);
unsigned short port_number(const json& p, const char* name);
std::string next_runtime_scope();
struct PreviewState {
  json dataset = nullptr;
  json replay = json::array();
  std::optional<data::v1::CsvSnapshot> source;
};
struct Application::Impl {
  std::unique_ptr<ResearchClient> research;
  std::unique_ptr<StrategyClient> strategy;
  json research_result = nullptr;
  std::unique_ptr<MarketClient> market;
  std::unique_ptr<TradingClient> paper;
  std::map<std::string, std::unique_ptr<NodeClient>> nodes;
  json firewall_plan = nullptr, firewall_parameters = nullptr, ssh_key = nullptr,
       agent_program = nullptr;
  std::chrono::steady_clock::time_point firewall_expiry{};
  Runtime core{next_runtime_scope()};
  ResourceRegistry::Scope scope = core.resources().create_scope("terminal");
  // Serializes every operation touching services; see Application.
  std::mutex operations, cache_mutex;
  json cache = nullptr;
  Impl();
  json snapshot();
  json dispatch(const json& request);
  json inspect(const json& params);
  // One registration per product area; each grants its capability and adds
  // commands before the runtime seals at start.
  void register_paper_commands();
  void register_node_commands();
  void register_research_commands();
  void register_strategy_commands();
  void register_market_commands();
};
} // namespace asterion::terminal
