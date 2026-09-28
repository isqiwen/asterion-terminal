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
#include <atomic>
#include <condition_variable>
#include <map>
#include <thread>
#include <optional>

namespace asterion::terminal {
using nlohmann::json;
using namespace asterion;
void fields(const json& object, std::initializer_list<std::string_view> names);
json risk_parameters(const json& p);
// The eight cost fields of a paper account request (per-lot and notional rates).
json cost_parameters(const json& p);
// fields() for a request that also carries every cost field.
void fields_with_costs(const json& object, std::initializer_list<std::string_view> names);
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
  // Serializes every operation touching service clients; see Application.
  std::mutex operations;
  // Incremented by every command under `operations`; a background refresh
  // that overlapped a command is discarded instead of publishing older state.
  std::uint64_t mutations = 0;
  // True while a command (not the background refresher) holds `operations`.
  // Probes return the published snapshot only then; a refresher step is
  // short (one client call) and worth waiting for.
  std::atomic<bool> command_running{false};
  // Published snapshot, guarded by cache_mutex. runtime.snapshot only reads it.
  std::mutex cache_mutex;
  std::condition_variable_any refresh_wake;
  json cache = nullptr;
  std::uint64_t revision = 0;
  std::int64_t refreshed_at_ms = 0;
  Impl();
  ~Impl();
  // Full snapshot with one RPC per service; caller holds `operations`.
  json snapshot();
  // Service-dependent parts, gathered one client call at a time.
  struct Parts {
    json paper = nullptr, connection = nullptr, process = nullptr, research = nullptr,
         strategy = nullptr, market = nullptr, nodes = json::array();
  };
  Parts gather_parts(bool hold_between_calls);
  json compose(const Parts& parts);
  void publish(json snapshot);
  json read_published(const json& params);
  void refresh_loop(std::stop_token stop);
  json dispatch(const json& request);
  json inspect(const json& params);
  // One registration per product area; each grants its capability and adds
  // commands before the runtime seals at start.
  void register_paper_commands();
  void register_node_commands();
  void register_research_commands();
  void register_strategy_commands();
  void register_market_commands();
  // Declared last: stopped and joined before any state it reads is destroyed.
  std::jthread refresher;
};
} // namespace asterion::terminal
