#pragma once
// Internal to the Terminal application: shared state of Application and the
// per-domain command registrations (commands_*.cpp). Not a public API.
#include "terminal_application.hpp"
#include <asterion/domain/history_identity.hpp>
#include "market_client.hpp"
#include "moving_average.hpp"
#include "node_client.hpp"
#include "data_connections.hpp"
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
std::string text(const json& object, const char* name, bool allow_empty = false);
unsigned short port_number(const json& p, const char* name);
std::string next_runtime_scope();
data::v1::DailyPageQuery daily_page_query(const json& params);
data::v1::MinutePageQuery minute_page_query(const json& params);
// Bars selected for paper trading, backtests, factors and strategy runs:
// completed downloads resolved by the research service, never local files.
struct DatasetSelection {
  data::v1::BarDatasetRequest request;
  data::v1::BarDataset dataset;
  json summary;
};
struct Application::Impl {
  std::shared_ptr<ResearchClient> research;
  std::atomic<std::uint64_t> research_generation{0};
  std::unique_ptr<StrategyClient> strategy;
  json native_plugins = nullptr;
  // Remembered credentials live in the keychain; the helper is found next to
  // the Terminal programs or through ASTERION_KEYCHAIN_EXECUTABLE.
  DataConnections data_connections{local_node_directory() / "data-connections",
                                   keychain_store(keychain_helper())};
  json connection_verification = nullptr;
  DataConnection resolve_data_connection(const std::string& id, const std::string& revision,
                                         const std::string& source);
  json research_result = nullptr;
  std::optional<DatasetSelection> selection;
  // The selected dataset or an actionable error; never an empty stand-in.
  const DatasetSelection& selected() const;
  std::vector<HistoryListing> history_contracts;
  std::string history_source;
  std::string history_connection, history_connection_revision;
  std::string history_exchange, history_product;
  std::int64_t history_cutoff = 0;
  std::shared_ptr<MarketClient> market;
  std::unique_ptr<TradingClient> paper;
  // Shared so long node I/O can keep its client while the map changes.
  std::map<std::string, std::shared_ptr<NodeClient>> nodes;
  json firewall_plan = nullptr, firewall_parameters = nullptr, ssh_key = nullptr,
       agent_program = nullptr;
  std::chrono::steady_clock::time_point firewall_expiry{};
  // The desktop app provides a log directory; other hosts log to stderr only.
  Runtime core{next_runtime_scope(), std::make_shared<SystemClock>(), [] {
                 auto file = process_logger("terminal");
                 return file ? file : std::make_shared<Logger>();
               }()};
  ResourceRegistry::Scope scope = core.resources().create_scope("terminal");
  // Serializes mutable client selection and Runtime dispatch. Minute-page I/O
  // uses a captured shared client outside this lock; see Application.
  std::mutex operations;
  // Incremented by every command under `operations`; a background refresh
  // that overlapped a command is discarded instead of publishing older state.
  std::uint64_t mutations = 0;
  // True while a command (not the background refresher) holds `operations`.
  // Probes return the published snapshot only then; a refresher step is
  // short (one client call) and worth waiting for.
  std::atomic<bool> command_running{false};
  // The dispatch lock of the command running on this thread, if any.
  std::unique_lock<std::mutex>* operation_lock = nullptr;
  // Serializes long node operations (SSH, uploads, upgrades) among themselves;
  // they run without `operations`, so other commands proceed meanwhile.
  std::mutex node_operations;
  // Runs `io` without the client-operation lock and relocks before returning.
  // Captured clients must be shared pointers; state is read again afterwards.
  template <class F> decltype(auto) without_operations(F&& io) {
    std::unique_lock node(node_operations, std::try_to_lock);
    if (!node)
      throw Error(ErrorCode::conflict,
                  "another node operation is in progress; retry after it completes");
    auto* lock = operation_lock;
    if (!lock || !lock->owns_lock())
      throw std::logic_error("node operation outside a Terminal command");
    command_running = false;
    lock->unlock();
    struct Relock {
      Impl& self;
      std::unique_lock<std::mutex>& lock;
      ~Relock() {
        lock.lock();
        ++self.mutations;
        self.command_running = true;
      }
    } relock{*this, *lock};
    return io();
  }
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
  // One registration per product area; each grants its capability and adds
  // commands before the runtime seals at start.
  void register_paper_commands();
  void register_node_commands();
  void register_research_commands();
  void register_connection_commands();
  void register_strategy_commands();
  void register_market_commands();
  void trim_market(json& result, const json& params);
  // Declared last: stopped and joined before any state it reads is destroyed.
  std::jthread refresher;
};
} // namespace asterion::terminal
