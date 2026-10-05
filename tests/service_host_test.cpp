#include "timing.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/service_host.hpp>
#include <filesystem>
#include <fstream>
#include <optional>
#include <cstdlib>
#include <asterion/kernel/logger.hpp>
#include <future>
#include <gtest/gtest.h>
#include <thread>
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif
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

#ifndef _WIN32
TEST(ServiceSignalsDeathTest, BrokenPipeDoesNotKillTheServiceAndTerminationStillStopsIt) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  ASSERT_EXIT(
      {
        // Do not inherit SIG_IGN from a Python/Node parent and get a false pass.
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        if (::sigaction(SIGPIPE, &action, nullptr) != 0)
          _exit(10);
        service::reset_stop_request();
        service::install_stop_signals();
        int pipe[2];
        if (::pipe(pipe) != 0)
          _exit(11);
        ::close(pipe[0]);
        errno = 0;
        const auto written = ::write(pipe[1], "test", 4);
        const auto failure = errno;
        ::close(pipe[1]);
        if (written != -1 || failure != EPIPE || service::stop_requested())
          _exit(12);
        if (::raise(SIGTERM) != 0 || !service::stop_requested())
          _exit(13);
        _exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}
#endif

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
  // A failed assertion must still stop the host, or the future never completes.
  struct Stop {
    ~Stop() { service::request_stop(); }
  } stop;
  std::vector<ipc::Channel> clients;
  for (int i = 0; i < 4; ++i)
    clients.push_back(connect(endpoint));
  // Generous for a loaded machine; the property is that all four are admitted.
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (started < 4 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(5ms);
  ASSERT_EQ(started.load(), 4) << "idle workers must accept the whole burst";
  auto excess = connect(endpoint);
  // The host closes an excess peer at once, so either the write or the read fails.
  EXPECT_THROW(
      {
        excess.send("late", 1s);
        static_cast<void>(excess.receive(1s));
      },
      std::exception)
      << "no capacity left";
  for (int i = 0; i < 4; ++i) {
    clients[i].send(std::to_string(i), 1s);
    EXPECT_EQ(clients[i].receive(2s), "echo:" + std::to_string(i));
  }
  service::request_stop();
  EXPECT_TRUE(served.get());
}

TEST_F(ServiceHostTest, HandlerFailureIsLoggedWithoutPayloadAndOtherConnectionsStillWork) {
  const char* value = std::getenv("ASTERION_LOG_DIRECTORY");
  const std::optional<std::string> saved = value ? std::optional<std::string>(value) : std::nullopt;
  struct Restore {
    const std::optional<std::string>& saved;
    ~Restore() {
      if (saved)
        ::setenv("ASTERION_LOG_DIRECTORY", saved->c_str(), 1);
      else
        ::unsetenv("ASTERION_LOG_DIRECTORY");
    }
  } restore{saved};
  const auto logs = root / "logs";
  ASSERT_EQ(::setenv("ASTERION_LOG_DIRECTORY", logs.c_str(), 1), 0);
  service::HostOptions options;
  options.poll = 20ms;
  service::ServiceHost host(
      local(),
      [](service::Connection& connection, std::stop_token) {
        if (connection.receive(1s) == "fail")
          throw Error(ErrorCode::conflict, "sensitive-handler-detail");
        connection.send("ok", 1s);
      },
      options);
  auto served = std::async(std::launch::async, [&] { return host.run(); });
  struct Stop {
    ~Stop() { service::request_stop(); }
  } stop;
  auto bad = connect(endpoint);
  bad.send("fail", 1s);
  EXPECT_THROW(static_cast<void>(bad.receive(2s)), std::exception);
  auto good = connect(endpoint);
  good.send("ready", 1s);
  EXPECT_EQ(good.receive(2s), "ok");
  service::request_stop();
  EXPECT_TRUE(served.get());
  unsigned records = 0;
  for (const auto& entry : std::filesystem::directory_iterator(logs)) {
    std::ifstream stream(entry.path());
    std::string line;
    while (std::getline(stream, line)) {
      EXPECT_EQ(line.find("sensitive-handler-detail"), std::string::npos);
      const auto record = Json::parse(line);
      EXPECT_EQ(record.at("event"), "connection.handler_failed");
      EXPECT_EQ(record.at("fields").at("code"), "conflict");
      EXPECT_EQ(record.at("fields").at("failures"), 1);
      ++records;
    }
  }
  EXPECT_EQ(records, 1U);
}
