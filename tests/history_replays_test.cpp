#include "../apps/clients/terminal/native/history_replays.hpp"
#include "../apps/clients/terminal/native/node_client.hpp"
#include "../apps/clients/terminal/native/research_client.hpp"
#include <asterion/kernel/service_host.hpp>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "bar_fixture.hpp"
#include "journal_fixture.hpp"
#include "paper_record.hpp"
#include "paper_session.hpp"
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <gtest/gtest.h>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <future>
#include <fstream>
using namespace asterion;
namespace {
struct Directory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("asterion-usage-" + unique_process_id());
  Directory() { std::filesystem::create_directories(path / "accounts" / "paper"); }
  ~Directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path account(const std::string& name) {
    const auto result = path / "accounts" / "paper" / name;
    std::filesystem::create_directory(result);
    return result;
  }
};
Json manifest(bool dual = false) {
  auto data = test::dataset({test::flat("2026-09-25", 100, "100")});
  if (dual) {
    data.set_settlement_dataset_ids(0, std::string(64, 'a'));
    data.set_revision(protocol::bar_dataset_revision(data));
  }
  Json costs{{"margin_per_lot", "100"},     {"open_fee", "2"},
             {"close_today_fee", "3"},      {"close_yesterday_fee", "4"},
             {"margin_rate", "0"},          {"open_fee_rate", "0"},
             {"close_today_fee_rate", "0"}, {"close_yesterday_fee_rate", "0"}};
  return {{"version", 4},
          {"type", "historical_paper"},
          {"deposit", "10000"},
          {"risk",
           {{"max_order_quantity", "100"},
            {"max_gross_quantity", "100"},
            {"max_working_orders", std::uint64_t{100}}}},
          {"contracts",
           {{{"dataset", protocol::decode_bar_dataset(data)},
             {"cost_schedule", test::cost_schedule(costs)}}}}};
}
} // namespace
TEST(LocalReplayUsage, ClosedLedgerKeepsBothRolesAndBusyLedgerRemainsUnchecked) {
  Directory root;
  const auto closed = root.account("已停止");
  Json snapshot;
  {
    trading::PaperSession session(closed, manifest(true));
    snapshot = session.snapshot();
  }
  const auto before = sha256_file(closed / "journal.sqlite");
  const auto modified = std::filesystem::last_write_time(closed / "journal.sqlite");
  const auto active = root.account("运行中");
  trading::PaperSession running(active, manifest());
  const auto live_before = running.snapshot();
  const auto usage = terminal::local_replay_usage(root.path, std::string(64, 'a'));
  EXPECT_EQ(usage.at("checked"), 1);
  ASSERT_EQ(usage.at("references").size(), 1U);
  EXPECT_EQ(usage.at("references")[0].at("roles"), Json::array({"market", "settlement"}));
  ASSERT_EQ(usage.at("unavailable").size(), 1U);
  EXPECT_EQ(usage.at("unavailable")[0].at("name"), "运行中");
  EXPECT_EQ(usage.at("unavailable")[0].at("diagnostic"), "database is in use by another writer");
  EXPECT_EQ(sha256_file(closed / "journal.sqlite"), before);
  EXPECT_TRUE(std::filesystem::last_write_time(closed / "journal.sqlite") == modified);
  EXPECT_EQ(running.snapshot(), live_before);
  {
    trading::PaperSession restored(closed);
    EXPECT_EQ(restored.snapshot(), snapshot);
  }
  const auto absent = terminal::local_replay_usage(root.path, std::string(64, 'c'));
  EXPECT_TRUE(absent.at("references").empty());
  EXPECT_EQ(absent.at("unavailable").size(), 1U);
}
TEST(LocalReplayUsage, MissingCorruptUnsupportedAndLinkedLedgersAreNotNoReferences) {
  Directory root;
  const auto missing = root.account("missing");
  const auto corrupt = root.account("corrupt");
  {
    std::ofstream out(corrupt / "journal.sqlite");
    out << "not a database";
  }
  const auto unsupported = root.account("unsupported");
  {
    trading::PaperSession session(unsupported, manifest());
  }
  auto header = test::read_record(test::journal_record(unsupported, 0));
  header["engine"] = "unsupported-engine";
  test::write_record(test::journal_record(unsupported, 0), header);
  std::filesystem::create_directory_symlink(unsupported, root.path / "accounts/paper/linked");
  auto result = terminal::local_replay_usage(root.path, std::string(64, 'a'));
  EXPECT_EQ(result.at("checked"), 0);
  EXPECT_TRUE(result.at("references").empty());
  EXPECT_EQ(result.at("unavailable").size(), 4U);
  EXPECT_FALSE(std::filesystem::exists(missing / "journal.sqlite"));
  header["engine"] = trading::journal_engine;
  header["format"] = 4;
  test::write_record(test::journal_record(unsupported, 0), header);
  EXPECT_THROW(trading::read_paper_input(unsupported), std::invalid_argument);
  header["format"] = trading::journal_format;
  header["manifest"]["contracts"][0]["dataset"]["source_dataset_ids"] = Json::array({"invalid"});
  test::write_record(test::journal_record(unsupported, 0), header);
  EXPECT_THROW(trading::read_paper_input(unsupported), std::invalid_argument);
}
TEST(LocalReplayUsage, ReadOnlyDatabaseRefusesWritesAndMissingRootsAreNotCreated) {
  Directory root;
  auto result = terminal::local_replay_usage(root.path / "absent", std::string(64, 'a'));
  EXPECT_EQ(result.at("checked"), 0);
  EXPECT_FALSE(std::filesystem::exists(root.path / "absent"));
  const auto directory = root.account("closed");
  {
    trading::PaperSession session(directory, manifest());
  }
  const auto before = sha256_file(directory / "journal.sqlite");
  {
    sqlite::Database database(directory / "journal.sqlite", sqlite::Database::Access::read_only);
    EXPECT_THROW(database.execute("DELETE FROM records"), std::runtime_error);
  }
  EXPECT_EQ(before, sha256_file(directory / "journal.sqlite"));
  std::filesystem::create_directory_symlink(root.path, root.path / "linked-root");
  result = terminal::local_replay_usage(root.path / "linked-root", std::string(64, 'a'));
  EXPECT_TRUE(result.contains("error"));
}

TEST(LocalReplayUsage, OwnerResponsesValidateEveryIdentityAndNeverHideFailure) {
  using namespace std::chrono_literals;
  Directory root;
  const auto account = root.account("owner");
  const auto other = root.path / "other";
  std::filesystem::create_directory(other);
  const auto endpoint = "/tmp/ast-usage-" + unique_process_id() + ".sock";
  ipc::Listener listener(endpoint);
  terminal::ReplayOwner owner{account, "paper.owner", endpoint};
  for (const std::string fault :
       {"", "session", "correlation", "mode", "version", "dataset", "directory", "directory_nul",
        "revision", "error", "missing", "unknown", "timeout"}) {
    SCOPED_TRACE(fault);
    auto future = std::async(std::launch::async, [&] {
      return terminal::local_replay_usage(root.path, std::string(64, 'a'), {owner});
    });
    auto channel = listener.accept(2s);
    protocol::v1::Request request;
    ASSERT_TRUE(request.ParseFromString(channel.receive(2s)));
    ASSERT_TRUE(request.has_history_usage());
    protocol::v1::Response response;
    response.set_version(1);
    response.set_mode(protocol::v1::PAPER);
    response.set_session_id(request.session_id());
    response.set_correlation_id(request.correlation_id());
    auto* usage = response.mutable_history_usage();
    usage->set_dataset_id(request.history_usage().dataset_id());
    usage->set_dataset_revision(std::string(64, 'b'));
    usage->set_directory(account.string());
    usage->set_market(true);
    usage->set_settlement(true);
    if (fault == "session")
      response.set_session_id("other");
    if (fault == "correlation")
      response.set_correlation_id("other");
    if (fault == "mode")
      response.set_mode(protocol::v1::LIVE);
    if (fault == "version")
      response.set_version(2);
    if (fault == "dataset")
      usage->set_dataset_id(std::string(64, 'c'));
    if (fault == "directory")
      usage->set_directory(other.string());
    if (fault == "directory_nul")
      usage->set_directory(account.string() + std::string(1, '\0') + "other");
    if (fault == "revision")
      usage->set_dataset_revision("invalid");
    if (fault == "error") {
      response.mutable_error()->set_code("unavailable");
      response.mutable_error()->set_message("fixture unavailable");
    }
    if (fault == "missing")
      response.mutable_snapshot();
    if (fault == "unknown")
      response.GetReflection()->MutableUnknownFields(&response)->AddVarint(999, 1);
    if (fault != "timeout")
      channel.send(response.SerializeAsString(), 2s);
    ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    const auto result = future.get();
    if (fault.empty()) {
      EXPECT_EQ(result.at("checked"), 1);
      ASSERT_EQ(result.at("references").size(), 1U);
      EXPECT_EQ(result.at("references")[0].at("roles"), Json::array({"market", "settlement"}));
      EXPECT_TRUE(result.at("unavailable").empty());
    } else {
      EXPECT_EQ(result.at("checked"), 0);
      EXPECT_TRUE(result.at("references").empty());
      EXPECT_EQ(result.at("unavailable").size(), 1U);
    }
  }
  const auto ambiguous =
      terminal::local_replay_usage(root.path, std::string(64, 'a'), {owner, owner});
  EXPECT_EQ(ambiguous.at("unavailable").size(), 1U);
  owner.endpoint += ".missing";
  const auto disconnected = terminal::local_replay_usage(root.path, std::string(64, 'a'), {owner});
  EXPECT_EQ(disconnected.at("checked"), 0);
  EXPECT_EQ(disconnected.at("unavailable").size(), 1U);
  EXPECT_TRUE(std::filesystem::is_empty(account));
}

namespace {
std::uint16_t reserve_port() {
  const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket < 0)
    throw std::runtime_error("test socket failed");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(socket);
    throw std::runtime_error("test bind failed");
  }
  socklen_t size = sizeof(address);
  const auto result = ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size);
  ::close(socket);
  if (result != 0)
    throw std::runtime_error("test socket address failed");
  return ntohs(address.sin_port);
}
} // namespace
TEST(RemoteReplayUsage, TlsInventoryAndQueriesPreserveUninspectedStoppedAndFailedAccounts) {
  using namespace std::chrono_literals;
  Directory root;
  ChildProcess certificates(current_executable().parent_path() / "asterion_test_certificates",
                            {root.path.string()});
  ASSERT_TRUE(certificates.wait(10s));
  ASSERT_EQ(certificates.exit_code(), 0);
  const auto identity = [&](const std::string& name) {
    return ipc::TlsIdentity{(root.path / "ca.crt").string(), (root.path / (name + ".crt")).string(),
                            (root.path / (name + ".key")).string()};
  };
  const auto port = reserve_port(), agent_port = reserve_port(), research_port = reserve_port();
  std::atomic<unsigned> mutations{0};
  std::atomic<bool> wrong_directory{false}, uninitialized{false};
  service::reset_stop_request();
  service::ServiceHost paper(
      {{}, "127.0.0.1", port, identity("server")},
      [&](service::Connection& channel, std::stop_token) {
        protocol::v1::Request request;
        if (!request.ParseFromString(channel.receive(2s)) || !request.has_history_usage()) {
          ++mutations;
          return;
        }
        protocol::v1::Response response;
        response.set_version(1);
        response.set_session_id(request.session_id());
        response.set_mode(protocol::v1::PAPER);
        response.set_correlation_id(request.correlation_id());
        if (uninitialized) {
          response.mutable_error()->set_code("unavailable");
          response.mutable_error()->set_message("session is not initialized");
        } else {
          auto* usage = response.mutable_history_usage();
          usage->set_dataset_id(request.history_usage().dataset_id());
          usage->set_dataset_revision(std::string(64, 'b'));
          usage->set_directory(wrong_directory ? "/srv/different" : "/srv/remote-only/ledger");
          usage->set_market(true);
          usage->set_settlement(true);
        }
        channel.send(response.SerializeAsString(), 2s);
      });
  std::atomic<bool> wrong_research{false};
  service::ServiceHost research(
      {{}, "127.0.0.1", research_port, identity("server")},
      [&](service::Connection& channel, std::stop_token) {
        research::v1::TaskRequest request;
        if (!request.ParseFromString(channel.receive(2s)) || !request.has_history_usage()) {
          ++mutations;
          return;
        }
        research::v1::TaskResponse response;
        response.set_version(1);
        response.set_service_id(request.service_id());
        response.set_correlation_id(wrong_research ? "wrong" : request.correlation_id());
        auto* usage = response.mutable_history_usage();
        usage->set_dataset_id(request.history_usage().id());
        auto* row = usage->add_references();
        row->set_kind(data::v1::HISTORY_SAVED_DATASET);
        row->set_id("saved");
        row->add_roles(data::v1::HISTORY_MARKET);
        row->add_roles(data::v1::HISTORY_SETTLEMENT);
        channel.send(response.SerializeAsString(), 2s);
      });
  service::ServiceHost agent({{}, "127.0.0.1", agent_port, identity("server")},
                             [&](service::Connection& channel, std::stop_token) {
                               node::v1::Request request;
                               if (!request.ParseFromString(channel.receive(2s)) ||
                                   !request.has_status()) {
                                 ++mutations;
                                 return;
                               }
                               node::v1::Response response;
                               response.set_version(1);
                               response.set_correlation_id(request.correlation_id());
                               auto* status = response.mutable_status();
                               status->set_os("linux");
                               status->set_arch("x86_64");
                               status->set_instance_id("fixture");
                               status->set_version("0.1.0");
                               auto* running = status->add_services();
                               running->set_id("running");
                               running->set_kind(node::v1::PAPER_TRADING);
                               running->set_state("running");
                               running->set_port(port);
                               running->set_directory("/srv/remote-only/ledger");
                               auto* stopped = status->add_services();
                               stopped->set_id("stopped");
                               stopped->set_kind(node::v1::PAPER_TRADING);
                               stopped->set_state("stopped");
                               stopped->set_directory("/srv/stopped/ledger");
                               auto* research = status->add_services();
                               research->set_id("research");
                               research->set_kind(node::v1::TASK_SERVICE);
                               research->set_state("running");
                               research->set_port(research_port);
                               research->set_endpoint("/remote/socket/must-not-be-used");
                               auto* live = status->add_services();
                               live->set_id("live");
                               live->set_kind(node::v1::LIVE_TRADING);
                               channel.send(response.SerializeAsString(), 2s);
                             });
  auto research_run = std::async(std::launch::async, [&] { return research.run(); });
  auto paper_run = std::async(std::launch::async, [&] { return paper.run(); });
  auto agent_run = std::async(std::launch::async, [&] { return agent.run(); });
  struct Stop {
    std::future<bool>& a;
    std::future<bool>& b;
    std::future<bool>& c;
    ~Stop() {
      service::request_stop();
      a.wait();
      b.wait();
      c.wait();
      service::reset_stop_request();
    }
  } stop{paper_run, agent_run, research_run};
  terminal::NodeClient node({"remote-fixture", "localhost", agent_port, identity("client")});
  const auto services = node.history_inventory();
  ASSERT_EQ(services.size(), 3U);
  auto research_address = services.back().address;
  EXPECT_TRUE(research_address.endpoint.empty());
  const auto inspect_research = [&] {
    return terminal::ResearchClient::inspect_history_usage(research_address, std::string(64, 'a'),
                                                           std::chrono::steady_clock::now() + 2s);
  };
  EXPECT_EQ(inspect_research().at("references")[0].at("roles"),
            Json::array({"market", "settlement"}));
  wrong_research = true;
  EXPECT_THROW(inspect_research(), Error);
  wrong_research = false;
  research_address.tls = identity("expired");
  EXPECT_THROW(inspect_research(), std::exception);
  research_address.tls = identity("client");
  research_address.tls.ca_file = (root.path / "other.crt").string();
  EXPECT_THROW(inspect_research(), std::exception);

  std::vector<terminal::RemoteReplayOwner> inventory;
  for (const auto& service : services)
    if (service.kind == node::v1::PAPER_TRADING)
      inventory.push_back({service.directory, service.state, service.address});
  ASSERT_EQ(inventory.size(), 2U);
  const auto id = std::string(64, 'a');
  const auto usage = terminal::remote_replay_usage(inventory, id);
  EXPECT_EQ(usage.at("checked"), 1);
  ASSERT_EQ(usage.at("references").size(), 1U);
  EXPECT_EQ(usage.at("references")[0].at("roles"), Json::array({"market", "settlement"}));
  ASSERT_EQ(usage.at("unavailable").size(), 1U);
  EXPECT_EQ(usage.at("unavailable")[0].at("name"), "stopped");
  inventory.resize(1);
  wrong_directory = true;
  EXPECT_EQ(terminal::remote_replay_usage(inventory, id).at("checked"), 0);
  wrong_directory = false;
  uninitialized = true;
  const auto pending = terminal::remote_replay_usage(inventory, id);
  ASSERT_EQ(pending.at("unavailable").size(), 1U);
  EXPECT_EQ(pending.at("unavailable")[0].at("diagnostic"), "session is not initialized");
  uninitialized = false;
  for (const auto& name : {"stranger", "expired"}) {
    inventory[0].address.tls = identity(name);
    const auto rejected = terminal::remote_replay_usage(inventory, id);
    EXPECT_EQ(rejected.at("checked"), 0);
    EXPECT_EQ(rejected.at("unavailable").size(), 1U);
  }
  inventory[0].address.tls = identity("client");
  inventory[0].address.tls.ca_file = (root.path / "other.crt").string();
  EXPECT_EQ(terminal::remote_replay_usage(inventory, id).at("checked"), 0);
  inventory[0].address.tls = identity("client");
  inventory[0].directory.clear(); // A direct connection has no client-owned path.
  EXPECT_EQ(terminal::remote_replay_usage(inventory, id).at("checked"), 1);
  EXPECT_EQ(mutations, 0U);
}
