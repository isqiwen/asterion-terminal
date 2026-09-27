#include "timing.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/service_host.hpp>
#include <filesystem>
#include <future>
#include <gtest/gtest.h>
#include <thread>
using namespace asterion;
using namespace std::chrono_literals;
namespace {
struct ServiceHostTest : testing::Test {
  std::filesystem::path root;
  std::string endpoint, health;
  void SetUp() override {
    service::reset_stop_request();
#ifdef _WIN32
    root = std::filesystem::temp_directory_path() / ("ast-host-" + unique_process_id());
    endpoint = "asterion.host." + unique_process_id();
    health = endpoint + ".health";
#else
    root = std::filesystem::path("/tmp") / ("ast-host-" + unique_process_id().substr(0, 12));
    endpoint = (root / "service").string();
    health = (root / "health").string();
#endif
    std::filesystem::create_directory(root);
  }
  void TearDown() override {
    service::reset_stop_request();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  static ipc::Channel connect(const std::string& target) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    for (;;) {
      try {
        return ipc::Channel::connect(target, 500ms);
      } catch (const std::exception&) {
        if (std::chrono::steady_clock::now() >= deadline)
          throw;
        std::this_thread::sleep_for(20ms);
      }
    }
  }
  service::Transport local() const { return {endpoint, {}, 0, {}}; }
};
} // namespace

TEST(ServiceTransport, RequiresExactlyOneCompleteTransport) {
  EXPECT_NO_THROW((service::Transport{"/tmp/x", {}, 0, {}}.validate()));
  EXPECT_NO_THROW((service::Transport{{}, "127.0.0.1", 9000, {"/ca", "/cert", "/key"}}.validate()));
  EXPECT_THROW((service::Transport{}.validate()), std::invalid_argument);
  EXPECT_THROW(
      (service::Transport{"/tmp/x", "127.0.0.1", 9000, {"/ca", "/cert", "/key"}}.validate()),
      std::invalid_argument);
  EXPECT_THROW((service::Transport{{}, "127.0.0.1", 9000, {"/ca", "", "/key"}}.validate()),
               std::invalid_argument);
  EXPECT_THROW((service::Transport{{}, "127.0.0.1", 0, {"/ca", "/cert", "/key"}}.validate()),
               std::invalid_argument);
  EXPECT_THROW((service::Transport{"/tmp/x", {}, 0, {"/ca", {}, {}}}.validate()),
               std::invalid_argument);
}

TEST_F(ServiceHostTest, ServesIsolatesFailuresAndDrainsOnStop) {
  service::ServiceHost host(local(), [](service::Connection& connection, std::stop_token) {
    const auto frame = connection.receive(2s);
    if (frame == "fail")
      throw std::runtime_error("handler failure closes only this connection");
    connection.send("echo:" + frame, 2s);
  });
  auto served = std::async(std::launch::async, [&] { return host.run(); });
  {
    auto failing = connect(endpoint);
    failing.send("fail", 1s);
    EXPECT_THROW(static_cast<void>(failing.receive(2s)), std::exception);
  }
  auto channel = connect(endpoint);
  channel.send("ping", 1s);
  EXPECT_EQ(channel.receive(2s), "echo:ping");
  service::request_stop();
  ASSERT_EQ(served.wait_for(3s), std::future_status::ready);
  EXPECT_TRUE(served.get());
#ifndef _WIN32
  EXPECT_FALSE(std::filesystem::exists(endpoint)) << "listener socket removed after stop";
#endif
}

TEST_F(ServiceHostTest, ReportsIncompleteDrainWhileAHandlerIsStillReading) {
  std::promise<void> reading;
  service::HostOptions options;
  options.drain = 100ms;
  service::ServiceHost host(
      local(),
      [&](service::Connection& connection, std::stop_token) {
        reading.set_value();
        static_cast<void>(connection.receive(1500ms));
      },
      options);
  auto served = std::async(std::launch::async, [&] { return host.run(); });
  auto idle = connect(endpoint);
  reading.get_future().wait();
  const auto started = std::chrono::steady_clock::now();
  service::request_stop();
  EXPECT_FALSE(served.get());
  EXPECT_LT(std::chrono::steady_clock::now() - started, asterion::testing_support::bound(1s))
      << "stop does not wait for idle reads";
}

TEST_F(ServiceHostTest, TickRunsOnTheAcceptThreadWhileIdle) {
  std::atomic<int> ticks{0};
  service::HostOptions options;
  options.poll = 20ms;
  options.tick = [&] {
    if (++ticks >= 3)
      service::request_stop();
  };
  service::ServiceHost host(local(), [](service::Connection&, std::stop_token) {}, options);
  EXPECT_TRUE(host.run());
  EXPECT_GE(ticks.load(), 3);
}

TEST_F(ServiceHostTest, HealthChannelAnswersIndependently) {
  service::HealthChannel channel(health, [](const std::string& frame) {
    return frame == "hb" ? std::string("ok") : std::string();
  });
  auto client = connect(health);
  client.send("hb", 1s);
  EXPECT_EQ(client.receive(2s), "ok");
  auto ignored = connect(health);
  ignored.send("other", 1s);
  EXPECT_THROW(static_cast<void>(ignored.receive(1500ms)), std::exception);
}

TEST_F(ServiceHostTest, AdmitsABurstUpToCapacityAndRejectsBeyondIt) {
  service::HostOptions options;
  options.workers = 4;
  options.queue = 0;
  options.poll = 20ms;
  std::atomic<int> started{0};
  service::ServiceHost host(
      local(),
      [&](service::Connection& connection, std::stop_token) {
        ++started;
        connection.send("echo:" + connection.receive(3s), 1s);
      },
      options);
  auto served = std::async(std::launch::async, [&] { return host.run(); });
  std::vector<ipc::Channel> clients;
  for (int i = 0; i < 4; ++i)
    clients.push_back(connect(endpoint));
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (started < 4 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(5ms);
  ASSERT_EQ(started.load(), 4) << "idle workers must accept the whole burst";
  auto excess = connect(endpoint);
  excess.send("late", 1s);
  EXPECT_THROW(static_cast<void>(excess.receive(1s)), std::exception) << "no capacity left";
  for (int i = 0; i < 4; ++i) {
    clients[i].send(std::to_string(i), 1s);
    EXPECT_EQ(clients[i].receive(2s), "echo:" + std::to_string(i));
  }
  service::request_stop();
  EXPECT_TRUE(served.get());
}
