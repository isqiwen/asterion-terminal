#include "node_client.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/v1/node.pb.h>
#include <future>
#include <gtest/gtest.h>
#include <thread>
using namespace asterion;
using namespace asterion::terminal;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
TEST(NodeMaintenance, freezes_mutations_and_serializes_start) {
  const auto root = fs::temp_directory_path() /
                    ("ast-maint-" + unique_process_id().substr(0, 8));
  fs::create_directories(root / "ledger");
  fs::create_directories(root / "agent");
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  } cleanup{root};
#ifdef _WIN32
  const auto endpoint = "asterion.maintenance." + unique_process_id();
#else
  const auto endpoint = (root / "agent.sock").string();
#endif
  ChildProcess agent(
      ASTERION_AGENT_PATH,
      {"--directory", (root / "agent").string(), "--endpoint", endpoint});
  NodeEndpoint config{"local", "localhost", 0, {}, endpoint};
  std::unique_ptr<NodeClient> client;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!client) {
    try {
      client = std::make_unique<NodeClient>(config);
    } catch (const std::exception &) {
      ASSERT_FALSE(agent.exited());
      ASSERT_LT(std::chrono::steady_clock::now(), deadline);
      std::this_thread::sleep_for(50ms);
    }
  }
  const auto instance =
      client->status().at("health").at("instance_id").get<std::string>();
  const auto platform = current_platform();
  client->deploy(ASTERION_TRADE_PATH, platform.os, platform.arch, "paper", 0,
                 (root / "ledger").string());
  EXPECT_THROW(client->maintenance(true, "upgrade.test", instance),
               std::exception);
  client->action("paper", "stop");
  EXPECT_THROW(client->maintenance(true, "upgrade.test", "different"),
               std::exception);
  client->maintenance(true, "upgrade.test", instance);
  client->maintenance(true, "upgrade.test",
                      instance); // Exact retry is idempotent.
  EXPECT_THROW(client->maintenance(true, "other", instance), std::exception);
  NodeClient second(config);
  EXPECT_TRUE(second.status().at("health").at("maintenance").get<bool>());
  EXPECT_EQ(second.status().at("health").at("pid").get<std::uint64_t>(),
            agent.id());
  EXPECT_THROW(second.action("paper", "start"), std::exception);
  EXPECT_THROW(second.action("paper", "stop"), std::exception);
  EXPECT_THROW(second.maintenance(false, "wrong", instance), std::exception);
  // Verify every mutation family is rejected before payload handling.
  for (int operation = 0; operation < 7; ++operation) {
    node::v1::Request request;
    request.set_version(1);
    request.set_correlation_id("blocked");
    switch (operation) {
    case 0:
      request.mutable_upload();
      break;
    case 1:
      request.mutable_chunk();
      break;
    case 2:
      request.mutable_finish();
      break;
    case 3:
      request.mutable_deploy();
      break;
    case 4:
      request.mutable_update();
      break;
    case 5:
      request.mutable_firewall();
      break;
    default:
      request.mutable_action();
      break;
    }
    auto channel = ipc::Channel::connect(endpoint, 3s);
    channel.send(request.SerializeAsString(), 3s);
    node::v1::Response response;
    ASSERT_TRUE(response.ParseFromString(channel.receive(3s)));
    ASSERT_TRUE(response.has_error());
    EXPECT_NE(response.error().message().find("maintenance"),
              std::string::npos);
  }
  client->maintenance(false, "upgrade.test", instance);
  for (int i = 0; i < 4; ++i) {
    auto enter = std::async(std::launch::async, [&] {
      try {
        client->maintenance(true, "race", instance);
        return std::string{};
      } catch (const std::exception &error) {
        return std::string(error.what());
      }
    });
    auto start = std::async(std::launch::async, [&] {
      try {
        second.action("paper", "start");
        return std::string{};
      } catch (const std::exception &error) {
        return std::string(error.what());
      }
    });
    const auto enter_error = enter.get(), start_error = start.get();
    const bool entered = enter_error.empty(), started = start_error.empty();
    ASSERT_NE(entered, started)
        << "maintenance: " << enter_error << "; start: " << start_error;
    NodeClient observed(config);
    const auto health = observed.status().at("health");
    EXPECT_EQ(health.at("maintenance").get<bool>(), entered);
    EXPECT_EQ(health.at("services").at(0).at("desired_running").get<bool>(),
              started);
    if (entered)
      client->maintenance(false, "race", instance);
    else
      second.action("paper", "stop");
  }
}

TEST(NodeMaintenance,
     AcknowledgedMutationIsNotReportedFailedWhenStatusReadFails) {
  for (const bool maintenance : {true, false}) {
    const auto root = fs::temp_directory_path() /
                      ("ast-ack-" + unique_process_id().substr(0, 8));
    fs::create_directory(root);
    struct Cleanup {
      fs::path root;
      ~Cleanup() {
        std::error_code ec;
        fs::remove_all(root, ec);
      }
    } cleanup{root};
#ifdef _WIN32
    const auto endpoint = "asterion.ack." + unique_process_id();
#else
    const auto endpoint = (root / "test.sock").string();
#endif
    ipc::Listener listener(endpoint);
    auto server = std::async(std::launch::async, [&] {
      for (int i = 0; i < 3; ++i) {
        auto channel = listener.accept(3s);
        node::v1::Request request;
        if (!request.ParseFromString(channel.receive(3s)))
          throw std::runtime_error("invalid test request");
        node::v1::Response response;
        response.set_version(1);
        response.set_correlation_id(request.correlation_id());
        if (i == 0) {
          EXPECT_TRUE(request.has_status());
          auto *status = response.mutable_status();
          status->set_instance_id("test-instance");
          status->set_os("test-os");
          status->set_arch("test-arch");
        } else if (i == 1) {
          EXPECT_TRUE(maintenance ? request.has_maintenance()
                                  : request.has_action());
          response.mutable_accepted();
        } else {
          EXPECT_TRUE(request.has_status());
          // The peer has acknowledged the mutation, then the status response is
          // unavailable. Do not resend the mutation or invent fresh health.
          channel.close();
          continue;
        }
        channel.send(response.SerializeAsString(), 3s);
      }
    });
    NodeClient client(NodeEndpoint{"local", "localhost", 0, {}, endpoint});
    if (maintenance)
      EXPECT_NO_THROW(client.maintenance(true, "operation", "test-instance"));
    else
      EXPECT_NO_THROW(client.action("paper", "start"));
    server.get();
    const auto state = client.status();
    EXPECT_EQ(state.at("state"), "unreachable");
    EXPECT_TRUE(state.at("error").get<std::string>().starts_with("Agent status receive: "));
    EXPECT_EQ(state.at("health").at("instance_id"), "test-instance");
  }
}

TEST(NodeMaintenance, SilentLocalPeerDoesNotBlockHealthyStatus) {
  const auto root = fs::temp_directory_path() /
                    ("ast-slow-" + unique_process_id().substr(0, 8));
  fs::create_directory(root);
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  } cleanup{root};
#ifdef _WIN32
  const auto endpoint = "asterion.slow." + unique_process_id();
#else
  const auto endpoint = (root / "agent.sock").string();
#endif
  ChildProcess agent(ASTERION_AGENT_PATH,
                     {"--directory", root.string(), "--endpoint", endpoint});
  NodeEndpoint config{"local", "localhost", 0, {}, endpoint};
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  for (;;) {
    try {
      NodeClient ready(config);
      break;
    } catch (const Error &) {
      ASSERT_FALSE(agent.exited());
      ASSERT_LT(std::chrono::steady_clock::now(), deadline);
      std::this_thread::sleep_for(20ms);
    }
  }
  auto silent = ipc::Channel::connect(endpoint, 1s);
  std::unique_ptr<NodeClient> healthy;
  ASSERT_NO_THROW(healthy = std::make_unique<NodeClient>(config));
  EXPECT_EQ(healthy->status().at("state"), "online");
  EXPECT_EQ(healthy->status().at("health").at("pid").get<std::uint64_t>(),
            agent.id());
}
