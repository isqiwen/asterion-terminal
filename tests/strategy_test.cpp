#include "session.hpp"
#include <asterion/foundation/decimal.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/v1/node.pb.h>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#endif
using namespace asterion;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace wire = strategy::v1;
namespace {
struct Directory {
#ifdef _WIN32
  fs::path path = fs::temp_directory_path() / ("asterion-strategy-" + unique_process_id());
#else
  fs::path path = fs::path("/tmp") / ("ast-s-" + unique_process_id().substr(0, 12));
#endif
  Directory() { fs::create_directory(path); }
  ~Directory() {
    std::error_code e;
    fs::remove_all(path, e);
  }
};
wire::Config config() {
  wire::Config c;
  c.set_version(1);
  c.set_session_id("strategy.test");
  c.set_stream_id("market.test");
  c.set_plugin_id("asterion.strategy.cta.sma-long-flat");
  c.set_fast(1);
  c.set_slow(3);
  c.mutable_quantity()->set_units(Decimal::parse("2").raw());
  *c.mutable_contract() = protocol::encode_contract({{"venue", "SHFE"},
                                                     {"symbol", "rb2610"},
                                                     {"currency", "CNY"},
                                                     {"price_increment", "1"},
                                                     {"quantity_increment", "1"},
                                                     {"multiplier", "10"},
                                                     {"product", "rb"},
                                                     {"delivery_month", "2026-10"}});
  return c;
}
wire::Event event(std::uint64_t sequence, const std::string& price = "100") {
  wire::Event e;
  e.set_stream_id("market.test");
  e.set_sequence(sequence);
  e.mutable_tick()->set_timestamp_ns(static_cast<std::int64_t>(sequence));
  e.mutable_tick()->mutable_price()->set_units(Decimal::parse(price).raw());
  e.mutable_tick()->mutable_quantity()->set_units(Decimal::parse("1").raw());
  return e;
}
std::size_t records(const fs::path& p) {
  std::size_t count = 0;
  for (const auto& entry : fs::directory_iterator(p))
    if (entry.path().extension() == ".json")
      ++count;
  return count;
}
wire::Request request() {
  wire::Request r;
  r.set_version(1);
  r.set_session_id("strategy.test");
  r.set_correlation_id("request.test");
  return r;
}
wire::Response call(const std::string& endpoint, const wire::Request& request) {
  ipc::Channel channel;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  for (;;) {
    try {
      channel = ipc::Channel::connect(endpoint, 100ms);
      break;
    } catch (const std::exception&) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw;
      std::this_thread::sleep_for(20ms);
    }
  }
  channel.send(request.SerializeAsString(), 5s);
  wire::Response response;
  if (!response.ParseFromString(channel.receive(5s)))
    throw std::runtime_error("invalid strategy response");
  EXPECT_EQ(response.version(), 1U);
  EXPECT_EQ(response.session_id(), request.session_id());
  EXPECT_EQ(response.correlation_id(), request.correlation_id());
  return response;
}
} // namespace
TEST(Strategy, DurableReplayPreservesWarmupAndIntentIdentityWithoutDuplicateProcessing) {
  Directory dir;
  const auto c = config();
  std::string receipt, snapshot;
  {
    strategy::Session session(dir.path, c.session_id(), &c);
    EXPECT_FALSE(session.apply(event(1)).has_intent());
    EXPECT_FALSE(session.apply(event(2, "101")).has_intent());
    const auto output = session.apply(event(3, "102"));
    ASSERT_TRUE(output.has_intent());
    EXPECT_EQ(output.intent().target_quantity().units(), Decimal::parse("2").raw());
    receipt = output.SerializeAsString();
    snapshot = session.snapshot().SerializeAsString();
    EXPECT_EQ(session.apply(event(3, "102")).SerializeAsString(), receipt);
    EXPECT_FALSE(session.apply(event(1)).has_intent());
    EXPECT_EQ(records(dir.path), 4U);
    EXPECT_THROW((strategy::Session(dir.path, c.session_id())), std::exception);
  }
  {
    strategy::Session session(dir.path, c.session_id(), &c);
    EXPECT_EQ(session.snapshot().SerializeAsString(), snapshot);
    EXPECT_EQ(session.apply(event(3, "102")).SerializeAsString(), receipt);
    const auto flat = session.apply(event(4, "99"));
    ASSERT_TRUE(flat.has_intent());
    EXPECT_EQ(flat.intent().target_quantity().units(), 0);
    EXPECT_EQ(records(dir.path), 5U);
  }
}
TEST(Strategy, RejectedEventsAndConflictingCreateDoNotChangeState) {
  Directory dir;
  const auto c = config();
  strategy::Session session(dir.path, c.session_id(), &c);
  session.apply(event(1));
  const auto before = session.snapshot().SerializeAsString();
  EXPECT_THROW(session.apply(event(3)), std::invalid_argument);
  EXPECT_THROW(session.apply(event(1, "101")), std::invalid_argument);
  auto wrong = event(2);
  wrong.set_stream_id("other");
  EXPECT_THROW(session.apply(wrong), std::invalid_argument);
  wrong = event(2);
  wrong.mutable_tick()->clear_price();
  EXPECT_THROW(session.apply(wrong), std::invalid_argument);
  wrong = event(2, "100.5");
  EXPECT_THROW(session.apply(wrong), std::invalid_argument);
  wrong = event(2);
  wrong.mutable_tick()->set_timestamp_ns(0);
  EXPECT_THROW(session.apply(wrong), std::invalid_argument);
  wrong = event(10001);
  EXPECT_THROW(session.apply(wrong), std::invalid_argument);
  wrong = event(2);
  wrong.GetReflection()->MutableUnknownFields(&wrong)->AddVarint(99, 1);
  EXPECT_THROW(session.apply(wrong), std::invalid_argument);
  auto changed = c;
  changed.set_fast(2);
  EXPECT_THROW(session.verify_config(changed), std::invalid_argument);
  EXPECT_EQ(session.snapshot().SerializeAsString(), before);
  EXPECT_EQ(records(dir.path), 2U);
  EXPECT_FALSE(session.recovery_required());
  // Equal timestamps and identical ticks with distinct sequence numbers
  // survive.
  auto same = event(1);
  same.set_sequence(2);
  EXPECT_FALSE(session.apply(same).has_intent());
  same.set_sequence(3);
  EXPECT_EQ(session.apply(same).intent().target_quantity().units(), 0);
}
TEST(Strategy, CorruptIntentAndInterruptedWritesArePreservedAndRejected) {
  Directory dir;
  const auto c = config();
  {
    strategy::Session session(dir.path, c.session_id(), &c);
    session.apply(event(1));
    session.apply(event(2));
    session.apply(event(3, "102"));
  }
  const auto file = dir.path / "00000003.json";
  Json record;
  {
    std::ifstream in(file);
    in >> record;
  }
  record["receipt"]["intent"]["target_quantity"] = 0;
  {
    std::ofstream out(file);
    out << record.dump();
  }
  EXPECT_THROW((strategy::Session(dir.path, c.session_id())), std::invalid_argument);
  EXPECT_EQ(records(dir.path), 4U);
  Directory pending;
  {
    std::ofstream out(pending.path / "pending.tmp");
    out << "incomplete";
  }
  EXPECT_THROW((strategy::Session(pending.path, c.session_id(), &c)), std::invalid_argument);
  EXPECT_TRUE(fs::exists(pending.path / "pending.tmp"));
  EXPECT_EQ(records(pending.path), 0U);
  Directory invalid;
  auto bad = c;
  bad.set_fast(4);
  EXPECT_THROW((strategy::Session(invalid.path, c.session_id(), &bad)), std::invalid_argument);
  EXPECT_TRUE(fs::is_empty(invalid.path));
}
TEST(Strategy, FailedDurableWritePoisonsSessionBeforeAcknowledgement) {
  Directory dir;
  const auto c = config();
  strategy::Session session(dir.path, c.session_id(), &c);
  session.apply(event(1));
  fs::create_directory(dir.path / "pending.tmp");
  EXPECT_THROW(session.apply(event(2)), std::exception);
  EXPECT_TRUE(session.recovery_required());
  EXPECT_EQ(session.snapshot().processed(), 1U);
  EXPECT_THROW(session.apply(event(1)), std::runtime_error);
  EXPECT_EQ(records(dir.path), 2U);
  EXPECT_TRUE(fs::is_directory(dir.path / "pending.tmp"));
}
TEST(Strategy, IndependentProcessRecoversAndRetriesAfterClientDisconnect) {
  Directory dir;
  const auto c = config();
#ifdef _WIN32
  const auto endpoint = "asterion-strategy-" + unique_process_id();
  const auto health_endpoint = endpoint + "-health";
#else
  const auto endpoint = (dir.path / "events.sock").string();
  const auto health_endpoint = (dir.path / "health.sock").string();
#endif
  fs::create_directory(dir.path / "journal");
  const std::vector<std::string> args{
      "--session",  c.session_id(), "--directory",       (dir.path / "journal").string(),
      "--endpoint", endpoint,       "--health-endpoint", health_endpoint};
  std::string receipt;
  {
    ChildProcess child(ASTERION_STRATEGY_PATH, args);
    auto req = request();
    req.mutable_heartbeat();
    EXPECT_FALSE(call(health_endpoint, req).health().initialized());
    req = request();
    *req.mutable_create() = c;
    ASSERT_TRUE(call(endpoint, req).has_snapshot());
    ASSERT_TRUE(call(endpoint, req).has_snapshot());
    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
      req = request();
      *req.mutable_event() = event(seq, std::to_string(100 + seq));
      auto response = call(endpoint, req);
      ASSERT_TRUE(response.has_receipt()) << response.error().message();
      receipt = response.receipt().SerializeAsString();
    }
    // Lose the response to a mutation: a retry must acknowledge the committed
    // event, rather than running the plugin again or releasing session
    // ownership.
    req = request();
    *req.mutable_event() = event(4, "99");
    {
      auto channel = ipc::Channel::connect(endpoint, 5s);
      channel.send(req.SerializeAsString(), 5s);
    }
    auto replay = call(endpoint, req);
    ASSERT_TRUE(replay.has_receipt());
    receipt = replay.receipt().SerializeAsString();
    req = request();
    req.mutable_snapshot();
    EXPECT_EQ(call(endpoint, req).snapshot().processed(), 4U);
  }
#ifndef _WIN32
  // Supervisor reclaims only endpoints of the child it has already reaped.
  fs::remove(endpoint);
  fs::remove(health_endpoint);
#endif
  {
    ChildProcess child(ASTERION_STRATEGY_PATH, args);
    auto req = request();
    *req.mutable_event() = event(4, "99");
    EXPECT_EQ(call(endpoint, req).receipt().SerializeAsString(), receipt);
    req = request();
    *req.mutable_event() = event(5, "98");
    auto response = call(endpoint, req);
    ASSERT_TRUE(response.has_receipt());
    EXPECT_EQ(response.receipt().intent().target_quantity().units(), 0);
    req = request();
    req.mutable_heartbeat();
    EXPECT_TRUE(call(health_endpoint, req).health().initialized());
  }
}

TEST(StrategyAgent, DeploymentRestartAndDesiredStateAreIndependentOfClients) {
  Directory root;
  const auto state = root.path / "agent";
  fs::create_directory(state);
#ifdef _WIN32
  const auto endpoint = "asterion-agent-strategy-" + unique_process_id();
#else
  const auto endpoint = (root.path / "agent.sock").string();
#endif
  const std::vector<std::string> args{"--directory", state.string(), "--endpoint", endpoint};
  auto agent = std::make_unique<ChildProcess>(ASTERION_AGENT_PATH, args);
  auto node_call = [&](node::v1::Request req) {
    req.set_version(1);
    req.set_correlation_id("test.strategy");
    ipc::Channel channel;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
      try {
        channel = ipc::Channel::connect(endpoint, 100ms);
        break;
      } catch (const std::exception&) {
        if (agent->exited() || std::chrono::steady_clock::now() >= deadline)
          throw;
        std::this_thread::sleep_for(20ms);
      }
    }
    channel.send(req.SerializeAsString(), 5s);
    node::v1::Response response;
    if (!response.ParseFromString(channel.receive(5s)))
      throw std::runtime_error("invalid Agent response");
    protocol::validate_message(response);
    EXPECT_EQ(response.version(), 1U);
    EXPECT_EQ(response.correlation_id(), req.correlation_id());
    return response;
  };
  const auto hash = sha256_file(ASTERION_STRATEGY_PATH);
  node::v1::Request req;
  auto* upload = req.mutable_upload();
  upload->set_sha256(hash);
  upload->set_size(fs::file_size(ASTERION_STRATEGY_PATH));
  upload->set_os(current_platform().os);
  upload->set_arch(current_platform().arch);
  ASSERT_TRUE(node_call(req).has_accepted());
  std::ifstream binary(ASTERION_STRATEGY_PATH, std::ios::binary);
  std::string buffer(1024 * 1024, '\0');
  std::uint64_t offset = 0;
  while (binary.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
         binary.gcount()) {
    req.Clear();
    auto* chunk = req.mutable_chunk();
    chunk->set_sha256(hash);
    chunk->set_offset(offset);
    chunk->set_data(buffer.data(), static_cast<std::size_t>(binary.gcount()));
    ASSERT_TRUE(node_call(req).has_accepted());
    offset += static_cast<std::uint64_t>(binary.gcount());
  }
  req.Clear();
  req.mutable_finish()->set_sha256(hash);
  ASSERT_TRUE(node_call(req).has_accepted());
  req.Clear();
  auto* deploy = req.mutable_deploy();
  deploy->set_service_id("strategy-test");
  deploy->set_sha256(hash);
  deploy->set_kind(node::v1::STRATEGY);
  deploy->set_provider_artifact(hash);
  EXPECT_TRUE(node_call(req).has_error());
  deploy->clear_provider_artifact();
  deploy->set_worker_artifact(hash);
  EXPECT_TRUE(node_call(req).has_error());
  deploy->clear_worker_artifact();
  ASSERT_TRUE(node_call(req).has_accepted());
  auto status = [&] {
    node::v1::Request r;
    r.mutable_status();
    const auto response = node_call(r);
    if (!response.has_status() || response.status().services_size() != 1)
      throw std::runtime_error("managed strategy missing");
    return response.status().services(0);
  };
  auto wait_health = [&](const std::string& health) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    for (;;) {
      auto service = status();
      if (service.health() == health)
        return service;
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("strategy did not become " + health + ": " + service.error());
      std::this_thread::sleep_for(100ms);
    }
  };
  auto service = wait_health("awaiting_input");
  EXPECT_EQ(service.kind(), node::v1::STRATEGY);
  auto c = config();
  c.set_session_id("strategy-test");
  auto strategy_request = request();
  strategy_request.set_session_id(c.session_id());
  *strategy_request.mutable_create() = c;
  ASSERT_TRUE(call(service.endpoint(), strategy_request).has_snapshot());
  for (std::uint64_t seq = 1; seq <= 3; ++seq) {
    strategy_request = request();
    strategy_request.set_session_id(c.session_id());
    *strategy_request.mutable_event() = event(seq, std::to_string(100 + seq));
    ASSERT_TRUE(call(service.endpoint(), strategy_request).has_receipt());
  }
  service = wait_health("ready");
  const auto receipt = call(service.endpoint(), strategy_request).receipt().SerializeAsString();
  auto action = [&](node::v1::Action::Kind kind) {
    node::v1::Request r;
    r.mutable_action()->set_service_id(c.session_id());
    r.mutable_action()->set_kind(kind);
    const auto response = node_call(r);
    if (!response.has_accepted())
      throw std::runtime_error(response.error().message());
  };
  // Crash only this test's managed child; no client sends a restart request.
  const auto crashed_pid = service.pid();
#ifdef _WIN32
  const auto handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(crashed_pid));
  ASSERT_NE(handle, nullptr);
  const auto terminated = TerminateProcess(handle, 9);
  CloseHandle(handle);
  ASSERT_NE(terminated, 0);
#else
  ASSERT_EQ(::kill(static_cast<pid_t>(crashed_pid), SIGKILL), 0);
#endif
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  do {
    service = status();
    if (service.pid() && service.pid() != crashed_pid && service.health() == "ready")
      break;
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(100ms);
  } while (true);
  EXPECT_EQ(service.restarts(), 1U);
  EXPECT_EQ(call(service.endpoint(), strategy_request).receipt().SerializeAsString(), receipt);
  const auto pid = service.pid();
  action(node::v1::Action::RESTART);
  service = wait_health("ready");
  EXPECT_NE(service.pid(), pid);
  EXPECT_EQ(call(service.endpoint(), strategy_request).receipt().SerializeAsString(), receipt);
  // Restart the Agent itself. Its owned child exits and the persisted desired
  // state creates a new process that replays the same strategy journal.
  agent.reset();
#ifndef _WIN32
  fs::remove(endpoint);
#endif
  agent = std::make_unique<ChildProcess>(ASTERION_AGENT_PATH, args);
  service = wait_health("ready");
  EXPECT_EQ(call(service.endpoint(), strategy_request).receipt().SerializeAsString(), receipt);
  auto update_request = [&](const std::string& artifact, const std::string& expected) {
    node::v1::Request r;
    auto* u = r.mutable_update();
    u->set_service_id(c.session_id());
    u->set_expected_revision(expected);
    u->set_artifact(artifact);
    return r;
  };
  // Updating a live service is rejected without interrupting it.
  EXPECT_TRUE(node_call(update_request(hash, status().revision())).has_error());
  action(node::v1::Action::STOP);
  const auto stopped = status();
  const auto replacement = sha256_file(ASTERION_STRATEGY_REVISION_PATH);
  ASSERT_NE(replacement, hash);
  node::v1::Request update = update_request(replacement, stopped.revision());
  EXPECT_TRUE(node_call(update).has_error()); // no installed artifact yet
  req.Clear();
  auto* next_upload = req.mutable_upload();
  next_upload->set_sha256(replacement);
  next_upload->set_size(fs::file_size(ASTERION_STRATEGY_REVISION_PATH));
  next_upload->set_os(current_platform().os);
  next_upload->set_arch(current_platform().arch);
  ASSERT_TRUE(node_call(req).has_accepted());
  std::ifstream replacement_file(ASTERION_STRATEGY_REVISION_PATH, std::ios::binary);
  offset = 0;
  while (replacement_file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
         replacement_file.gcount()) {
    req.Clear();
    auto* chunk = req.mutable_chunk();
    chunk->set_sha256(replacement);
    chunk->set_offset(offset);
    chunk->set_data(buffer.data(), static_cast<std::size_t>(replacement_file.gcount()));
    ASSERT_TRUE(node_call(req).has_accepted());
    offset += static_cast<std::uint64_t>(replacement_file.gcount());
  }
  req.Clear();
  req.mutable_finish()->set_sha256(replacement);
  ASSERT_TRUE(node_call(req).has_accepted());
  EXPECT_TRUE(node_call(update_request(replacement, std::string(64, '0'))).has_error());
  auto invalid = update;
  invalid.mutable_update()->set_worker_artifact(hash);
  EXPECT_TRUE(node_call(invalid).has_error());
  invalid = update;
  invalid.mutable_update()->set_provider_artifact(hash);
  EXPECT_TRUE(node_call(invalid).has_error());
  const auto pending = state / "services" / c.session_id() / "service.pending";
  {
    std::ofstream file(pending);
    file << "interrupted test write";
  }
  EXPECT_TRUE(node_call(update).has_error());
  EXPECT_TRUE(fs::exists(pending));
  EXPECT_EQ(status().revision(), stopped.revision());
  agent.reset();
  agent = std::make_unique<ChildProcess>(ASTERION_AGENT_PATH, args);
  ASSERT_TRUE(agent->wait(5s));
  EXPECT_NE(agent->exit_code(), 0);
  EXPECT_TRUE(fs::exists(pending));
  agent.reset();
  fs::remove(pending); // Explicitly remove only the interrupted file injected above.
  agent = std::make_unique<ChildProcess>(ASTERION_AGENT_PATH, args);
  EXPECT_EQ(status().revision(), stopped.revision());
  const auto ledger_count = records(fs::path(stopped.directory()));
  ASSERT_TRUE(node_call(update).has_accepted());
  service = status();
  EXPECT_EQ(service.artifact(), replacement);
  EXPECT_FALSE(service.desired_running());
  EXPECT_EQ(service.pid(), 0U);
  EXPECT_EQ(service.directory(), stopped.directory());
  EXPECT_EQ(service.port(), stopped.port());
  EXPECT_NE(service.revision(), stopped.revision());
  EXPECT_EQ(records(fs::path(stopped.directory())), ledger_count);
  EXPECT_TRUE(node_call(update).has_error()); // stale confirmation after replacement
  action(node::v1::Action::START);
  service = wait_health("ready");
  EXPECT_EQ(call(service.endpoint(), strategy_request).receipt().SerializeAsString(), receipt);
  action(node::v1::Action::STOP);

  EXPECT_EQ(status().state(), "stopped");
  agent.reset();
#ifndef _WIN32
  fs::remove(endpoint);
#endif
  agent = std::make_unique<ChildProcess>(ASTERION_AGENT_PATH, args);
  EXPECT_EQ(status().artifact(), replacement);
  EXPECT_FALSE(status().desired_running());
  EXPECT_EQ(status().pid(), 0U);
  action(node::v1::Action::START);
  service = wait_health("ready");
  EXPECT_EQ(call(service.endpoint(), strategy_request).receipt().SerializeAsString(), receipt);
  action(node::v1::Action::STOP);
}

TEST(StrategyExecution, TwoProcessesRecoverAuthorizedTargetsWithoutDuplicateOrders) {
  Directory root;
  fs::create_directory(root.path / "strategy");
  fs::create_directory(root.path / "trading");
#ifdef _WIN32
  const auto endpoint = "asterion-strategy-exec-" + unique_process_id();
  const auto trade_endpoint = "asterion-trade-exec-" + unique_process_id();
#else
  const auto endpoint = (root.path / "s.sock").string();
  const auto trade_endpoint = (root.path / "t.sock").string();
#endif
  auto c = config();
  c.set_slow(2);
  c.mutable_quantity()->set_units(Decimal::parse("1").raw());
  protocol::v1::PaperInput input;
  *input.mutable_contract() = c.contract();
  input.mutable_deposit()->set_units(Decimal::parse("1000").raw());
  input.mutable_costs()->mutable_margin_per_lot()->set_units(Decimal::parse("100").raw());
  input.mutable_costs()->mutable_open_fee()->set_units(Decimal::parse("2").raw());
  input.mutable_costs()->mutable_close_today_fee()->set_units(Decimal::parse("3").raw());
  input.mutable_risk()->mutable_max_order_quantity()->set_units(10000000000LL);
  input.mutable_risk()->mutable_max_gross_quantity()->set_units(10000000000LL);
  input.mutable_risk()->set_max_working_orders(100);
  input.mutable_costs()->mutable_close_yesterday_fee()->set_units(Decimal::parse("4").raw());
  const std::vector<int> prices{100, 101, 100, 102, 99, 103};
  for (std::size_t i = 0; i < prices.size(); ++i)
    *input.add_ticks() = event(i + 1, std::to_string(prices[i])).tick();
  const auto revision = protocol::make_trade_dataset(input.contract(), input.ticks()).revision();
  std::unique_ptr<ChildProcess> strategy, trading;
  auto launch = [&] {
    strategy = std::make_unique<ChildProcess>(
        ASTERION_STRATEGY_PATH,
        std::vector<std::string>{"--session", c.session_id(), "--endpoint", endpoint, "--directory",
                                 (root.path / "strategy").string()});
    trading = std::make_unique<ChildProcess>(
        ASTERION_TRADE_PATH,
        std::vector<std::string>{"--session", "account", "--mode", "paper", "--endpoint",
                                 trade_endpoint, "--directory", (root.path / "trading").string()});
  };
  auto trade = [&](protocol::v1::Request r) {
    r.set_version(1);
    r.set_mode(protocol::v1::PAPER);
    r.set_session_id("account");
    r.set_correlation_id("trade.request");
    ipc::Channel channel;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
      try {
        channel = ipc::Channel::connect(trade_endpoint, 100ms);
        break;
      } catch (const std::exception&) {
        if (std::chrono::steady_clock::now() >= deadline)
          throw;
        std::this_thread::sleep_for(20ms);
      }
    }
    channel.send(r.SerializeAsString(), 2s);
    protocol::v1::Response response;
    if (!response.ParseFromString(channel.receive(2s)))
      throw std::runtime_error("invalid trading reply");
    if (response.has_error())
      throw std::runtime_error(response.error().message());
    return response.snapshot();
  };
  launch();
  auto sr = request();
  *sr.mutable_create() = c;
  ASSERT_TRUE(call(endpoint, sr).has_snapshot());
  protocol::v1::Request tr;
  *tr.mutable_create() = input;
  trade(tr);
  tr.Clear();
  tr.mutable_command()->set_request_id("grant");
  auto* grant = tr.mutable_command()->mutable_strategy_grant();
  grant->set_grant_id("grant.one");
  grant->set_strategy_id(c.session_id());
  grant->set_stream_id(c.stream_id());
  grant->set_dataset_revision(revision);
  grant->mutable_max_quantity()->set_units(Decimal::parse("1").raw());
  EXPECT_TRUE(trade(tr).strategy().active());
  protocol::v1::Snapshot account;
  for (std::size_t index = 0; index < prices.size(); ++index) {
    const auto seq = index + 1;
    tr.Clear();
    tr.mutable_command()->set_request_id("advance." + std::to_string(seq));
    tr.mutable_command()->mutable_advance();
    account = trade(tr);
    sr = request();
    *sr.mutable_event() = event(seq, std::to_string(prices[index]));
    auto receipt = call(endpoint, sr).receipt();
    if (!receipt.has_intent() || seq == prices.size())
      continue;
    const auto& intent = receipt.intent();
    tr.Clear();
    tr.mutable_command()->set_request_id(intent.id());
    auto* t = tr.mutable_command()->mutable_strategy_target();
    t->set_grant_id("grant.one");
    t->set_strategy_id(c.session_id());
    t->set_stream_id(c.stream_id());
    t->set_dataset_revision(revision);
    t->set_sequence(seq);
    t->set_timestamp_ns(intent.timestamp_ns());
    *t->mutable_target_quantity() = intent.target_quantity();
    const auto fills_before = account.fills_size();
    account = trade(tr);
    EXPECT_EQ(account.fills_size(), fills_before);
    if (seq == 3) {
      const auto before = account.SerializeAsString();
      strategy.reset();
      trading.reset();
#ifndef _WIN32
      fs::remove(endpoint);
      fs::remove(trade_endpoint);
#endif
      launch();
      EXPECT_EQ(call(endpoint, sr).receipt().SerializeAsString(), receipt.SerializeAsString());
      EXPECT_EQ(trade(tr).SerializeAsString(), before);
    }
  }
  EXPECT_EQ(account.orders_size(), 4);
  EXPECT_EQ(account.fills_size(), 4);
  EXPECT_EQ(account.positions_size(), 0);
  EXPECT_EQ(account.balance().units(), Decimal::parse("1050").raw());
  EXPECT_EQ(account.fees().units(), Decimal::parse("10").raw());
  tr.Clear();
  tr.mutable_command()->set_request_id("revoke");
  tr.mutable_command()->mutable_strategy_revoke()->set_grant_id("grant.one");
  EXPECT_FALSE(trade(tr).strategy().active());
}
