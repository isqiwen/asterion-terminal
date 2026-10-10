#include <asterion/kernel/rpc_host.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asio.hpp>
#include <cerrno>
#include <csignal>
#include <unistd.h>
#include <filesystem>
#include <tuple>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <thread>
using namespace asterion;
using namespace std::chrono_literals;
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

TEST(RpcHost, StreamingRepliesShareIoAndReturnToRequestsAfterTheirFinalFrame) {
  service::reset_stop_request();
  using Host = service::RpcHost;
  service::Transport transport;
  transport.endpoint = "/tmp/ast-stream-" + unique_process_id() + ".sock";
  Progress progress;
  Host server(
      transport,
      [](const auto&, std::string request) -> Host::Reply {
        if (request == "watch")
          return [sequence = 0]() mutable -> std::optional<Host::Message> {
            ++sequence;
            return Host::Message(std::to_string(sequence), sequence < 3);
          };
        return [request] { return std::optional<std::string>(request); };
      },
      {}, progress);
  auto running = std::async(std::launch::async, [&] { return server.run(); });
  struct Stop {
    ~Stop() { service::request_stop(); }
  } stop;
  auto watcher = ipc::Channel::connect(transport.endpoint, 1s);
  watcher.send("watch", 1s);
  EXPECT_EQ(watcher.receive(1s), "1");
  auto control = ipc::Channel::connect(transport.endpoint, 1s);
  control.send("control", 1s);
  EXPECT_EQ(control.receive(1s), "control");
  EXPECT_EQ(watcher.receive(1s), "2");
  EXPECT_EQ(watcher.receive(1s), "3");
  watcher.send("next request", 1s);
  EXPECT_EQ(watcher.receive(1s), "next request");
  service::request_stop();
  ASSERT_EQ(running.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(running.get());
}

TEST(RpcHost, PendingRepliesLeaveReadAndControlConnectionsAvailable) {
  service::reset_stop_request();
  const auto endpoint = "/tmp/ast-rpc-" + unique_process_id() + ".sock";
  std::promise<void> completed;
  auto completion = completed.get_future().share();
  std::size_t pending = 0;
  std::promise<void> resources;
  auto stopped = resources.get_future().share();
  using Host = service::RpcHost;
  auto stage = Host::Stage::running;
  Host::Options options;
  options.connections = 26;
  options.request_bytes = 128;
  options.payload_bytes = 128;
  options.drain = 2s;
  Host::LocalEndpoint health_endpoint{endpoint + ".health", {}, 4, true};
  health_endpoint.request_bytes = 16;
  health_endpoint.handler = [&](const Host::Peer&, std::string request) -> Host::Reply {
    if (request == "release")
      completed.set_value();
    return [value = std::to_string(static_cast<int>(stage))] {
      return std::optional<std::string>(value);
    };
  };
  options.local_endpoints.push_back(std::move(health_endpoint));
  std::promise<void> private_accepted;
  auto accepted = private_accepted.get_future();
  options.local_endpoints.push_back({endpoint + ".workers",
                                     [&](const Host::Peer&, std::string) -> Host::Reply {
                                       private_accepted.set_value();
                                       return [completion]() -> std::optional<std::string> {
                                         if (completion.wait_for(0ms) != std::future_status::ready)
                                           return {};
                                         return "private completed";
                                       };
                                     },
                                     2, false, 64});
  options.advance = [&](Host::Stage current) {
    stage = current;
    return stopped.wait_for(0ms) == std::future_status::ready;
  };
  service::Transport transport;
  transport.endpoint = endpoint;
  Progress progress;
  service::RpcHost server(
      transport,
      [&](const service::RpcHost::Peer& peer, std::string request) -> service::RpcHost::Reply {
        EXPECT_EQ(peer.role, ipc::PeerRole::local);
        if (request == "wait") {
          ++pending;
          return [completion]() -> std::optional<std::string> {
            if (completion.wait_for(0ms) != std::future_status::ready)
              return {};
            completion.get();
            return "done";
          };
        }
        return [value = std::to_string(pending)] { return std::optional<std::string>(value); };
      },
      std::move(options), progress);
  std::promise<bool> ended;
  auto drained = ended.get_future();
  std::jthread worker([&] {
    try {
      ended.set_value(server.run());
    } catch (...) {
      ended.set_exception(std::current_exception());
    }
  });
  struct Stop {
    ~Stop() { service::request_stop(); }
  } stop;
  std::vector<ipc::Channel> waiting;
  for (int i = 0; i < 24; ++i) {
    auto peer = ipc::Channel::connect(endpoint, 1s);
    peer.send("wait", 1s);
    waiting.push_back(std::move(peer));
  }
  auto control = ipc::Channel::connect(endpoint, 1s);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  std::string observed;
  do {
    control.send("read", 1s);
    observed = control.receive(1s);
  } while (observed != "24" && std::chrono::steady_clock::now() < deadline);
  ASSERT_EQ(observed, "24");
  control.send(std::string(32, 'x'), 1s);
  EXPECT_EQ(control.receive(1s), "24");
  // The 24 pending four-byte inputs retain 96 bytes even after their handlers
  // have taken ownership. 32 more fit; 33 must be rejected before allocation.
  auto excess = ipc::Channel::connect(endpoint, 1s);
  excess.send(std::string(33, 'x'), 1s);
  EXPECT_THROW(excess.receive(1s), Error);
  auto private_client = ipc::Channel::connect(endpoint + ".workers", 1s);
  private_client.send(std::string(32, 'x'), 1s);
  ASSERT_EQ(accepted.wait_for(1s), std::future_status::ready);
  auto oversized = ipc::Channel::connect(endpoint + ".health", 1s);
  oversized.send(std::string(32, 'x'), 1s);
  EXPECT_THROW(oversized.receive(1s), Error);
  auto health = ipc::Channel::connect(endpoint + ".health", 1s);
  health.send("read", 1s);
  EXPECT_EQ(health.receive(1s), std::to_string(static_cast<int>(Host::Stage::running)));
  service::request_stop();
  do {
    health.send("read", 1s);
    observed = health.receive(1s);
  } while (observed == "0" && std::chrono::steady_clock::now() < deadline);
  ASSERT_EQ(observed, std::to_string(static_cast<int>(Host::Stage::draining_replies)));
  health.send("release", 1s);
  static_cast<void>(health.receive(1s));
  for (auto& peer : waiting)
    EXPECT_EQ(peer.receive(1s), "done");
  EXPECT_EQ(private_client.receive(1s), "private completed");
  do {
    health.send("read", 1s);
    observed = health.receive(1s);
  } while (observed != "2" && std::chrono::steady_clock::now() < deadline);
  ASSERT_EQ(observed, std::to_string(static_cast<int>(Host::Stage::stopping_resources)));
  EXPECT_EQ(drained.wait_for(50ms), std::future_status::timeout);
  resources.set_value();
  ASSERT_EQ(drained.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(drained.get());
}

TEST(RpcHost, ShutdownDeadlineIncludesResourceOwnersAfterRepliesDrain) {
  service::reset_stop_request();
  service::Transport transport;
  transport.endpoint = "/tmp/ast-drain-" + unique_process_id() + ".sock";
  service::RpcHost::Options options;
  options.drain = 50ms;
  options.advance = [](service::RpcHost::Stage stage) {
    EXPECT_EQ(stage, service::RpcHost::Stage::stopping_resources);
    return false; // An SDK owner has not finished release.
  };
  Progress progress;
  service::RpcHost server(
      transport,
      [](const service::RpcHost::Peer&, std::string) -> service::RpcHost::Reply {
        return [] { return std::optional<std::string>("unused"); };
      },
      std::move(options), progress);
  service::request_stop();
  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(server.run());
  EXPECT_GE(std::chrono::steady_clock::now() - started, 50ms);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
}

TEST(RpcHost, IncompleteDrainRetainsAdmittedReplyUntilProcessExit) {
  service::reset_stop_request();
  service::Transport transport;
  transport.endpoint = "/tmp/ast-pending-" + unique_process_id() + ".sock";
  service::RpcHost::Options options;
  options.drain = 50ms;
  Progress progress;
  std::promise<void> accepted;
  std::weak_ptr<int> lifetime;
  service::RpcHost server(
      transport,
      [&](const auto&, std::string) -> service::RpcHost::Reply {
        auto state = std::make_shared<int>(1);
        lifetime = state;
        accepted.set_value();
        return [state] { return std::optional<std::string>{}; };
      },
      std::move(options), progress);
  auto running = std::async(std::launch::async, [&] { return server.run(); });
  struct Stop {
    ~Stop() { service::request_stop(); }
  } stop;
  auto client = ipc::Channel::connect(transport.endpoint, 2s);
  client.send("accepted file work", 2s);
  ASSERT_EQ(accepted.get_future().wait_for(2s), std::future_status::ready);
  service::request_stop();
  ASSERT_EQ(running.wait_for(1s), std::future_status::ready);
  EXPECT_FALSE(running.get());
  // Production exits here. Destroying the request earlier would invalidate
  // coroutine locals that a file worker may still be reading or writing.
  EXPECT_FALSE(lifetime.expired());
}

// A reply that waits for pool work leaves when the work is done, and a client
// on its owner's reactor receives it there. Neither side waits for a periodic
// check: the host's comes every 10 ms and would alone take 400 ms here.
TEST(RpcHost, FinishedPoolWorkIsAnsweredWithoutWaitingForAPeriodicCheck) {
  service::reset_stop_request();
  service::Transport transport;
  transport.endpoint = "/tmp/ast-prompt-" + unique_process_id() + ".sock";
  ThreadPool pool(1, 8, service::wake_io_owner);
  Progress progress;
  service::RpcHost server(
      transport,
      [&](const auto&, std::string request) -> service::RpcHost::Reply {
        auto work = std::make_shared<std::future<void>>(
            pool.submit([](std::stop_token) { std::this_thread::sleep_for(200us); }));
        return [work, request = std::move(request)]() -> std::optional<std::string> {
          if (work->wait_for(0ms) != std::future_status::ready)
            return {};
          return request;
        };
      },
      {}, progress);
  std::jthread worker([&] { static_cast<void>(server.run()); });
  struct Stop {
    ~Stop() { service::request_stop(); }
  } stop;
  ipc::Reactor reactor;
  ipc::RpcClient client(reactor, transport.endpoint, 1, PayloadBudget{128 * 1024 * 1024});
  constexpr int rounds = 40;
  const auto started = std::chrono::steady_clock::now();
  for (int round = 0; round < rounds; ++round) {
    auto reply = client.request(std::to_string(round), 5s);
    // The owner only waits on its reactor; nothing pumps the client by itself.
    while (reply.wait_for(0ms) != std::future_status::ready)
      reactor.wait(1s);
    ASSERT_EQ(*reply.get(), std::to_string(round));
  }
  EXPECT_LT(std::chrono::steady_clock::now() - started, rounds * 5ms);
}

TEST(Reactor, AWakeUpEndsTheNextWaitOrTheOneInProgress) {
  ipc::Reactor reactor;
  reactor.wake();
  reactor.wait(std::nullopt);
  std::atomic<bool> woken = false;
  std::jthread other([&] {
    std::this_thread::sleep_for(20ms);
    woken = true;
    reactor.wake();
  });
  // Without a limit only the wake-up can end this.
  while (!woken)
    reactor.wait(std::nullopt);
}

TEST(RpcClient, PendingReplyDoesNotBlockControlAndTimeoutNeverResends) {
  service::reset_stop_request();
  service::Transport transport;
  transport.endpoint = "/tmp/ast-client-" + unique_process_id() + ".sock";
  unsigned commands = 0;
  Progress progress;
  service::RpcHost server(
      transport,
      [&](const auto&, std::string request) -> service::RpcHost::Reply {
        if (request == "slow") {
          ++commands;
          return [] { return std::optional<std::string>{}; };
        }
        return [count = commands] { return std::optional<std::string>(std::to_string(count)); };
      },
      {}, progress);
  std::jthread worker([&] { static_cast<void>(server.run()); });
  struct Stop {
    ~Stop() { service::request_stop(); }
  } stop;
  ipc::RpcClient client(transport.endpoint, 2, PayloadBudget{128 * 1024 * 1024});
  const auto wait = [&](auto& future) {
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (future.wait_for(0ms) != std::future_status::ready &&
           std::chrono::steady_clock::now() < until) {
      client.poll();
      std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
  };
  auto slow = client.request("slow", 5s, std::chrono::steady_clock::now() + 300ms);
  auto control = client.request("read", 1s);
  EXPECT_THROW(client.request("over-capacity", 1s), Error);
  wait(control);
  // Read until the server has accepted the one slow command; connection arrival
  // order is not used as an ordering guarantee between separate requests.
  while (*control.get() == "0") {
    control = client.request("read", 1s);
    wait(control);
  }
  EXPECT_EQ(slow.wait_for(0ms), std::future_status::timeout);
  wait(slow);
  EXPECT_THROW(static_cast<void>(slow.get()), Error);
  control = client.request("read", 1s);
  wait(control);
  EXPECT_EQ(*control.get(), "1");
  auto expired = client.request("slow", 5s, std::chrono::steady_clock::now() - 1ms);
  EXPECT_THROW(static_cast<void>(expired.get()), Error);
  control = client.request("read", 1s);
  wait(control);
  EXPECT_EQ(*control.get(), "1");
  std::future<Payload> cancelled;
  {
    ipc::RpcClient retiring(transport.endpoint, 1, PayloadBudget{128 * 1024 * 1024});
    cancelled = retiring.request("read", 1s);
  }
  EXPECT_THROW(static_cast<void>(cancelled.get()), Error);
}

TEST(RpcClient, MaximumReplyCompletesWithinTheServiceSendDeadline) {
  service::reset_stop_request();
  service::Transport transport;
  transport.endpoint = "/tmp/ast-large-reply-" + unique_process_id() + ".sock";
  Progress progress;
  service::RpcHost::Options options;
  options.send = 3s; // Task service's actual reply budget.
  service::RpcHost server(
      transport,
      [](const auto&, std::string) -> service::RpcHost::Reply {
        return [] { return std::optional<std::string>(std::string(ipc::Channel::max_frame, 'x')); };
      },
      options, progress);
  std::jthread worker([&] { static_cast<void>(server.run()); });
  struct Stop {
    ~Stop() { service::request_stop(); }
  } stop;
  ipc::RpcClient client(transport.endpoint, 1, PayloadBudget{128 * 1024 * 1024});
  auto reply = client.request("maximum", 5s);
  const auto started = std::chrono::steady_clock::now();
  while (reply.wait_for(0ms) != std::future_status::ready)
    client.poll(10ms); // The worker's I/O and supervision cadence.
  RecordProperty("transfer_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count());
  const auto payload = reply.get();
  EXPECT_EQ(payload->size(), ipc::Channel::max_frame);
  EXPECT_EQ(payload->find_first_not_of('x'), std::string::npos);
}

TEST(RpcClient, StreamingFramesYieldToControlsAndReleaseTheirCapacityOnFailure) {
  service::reset_stop_request();
  service::Transport transport;
  transport.endpoint = "/tmp/ast-watch-" + unique_process_id() + ".sock";
  Progress progress;
  service::RpcHost server(
      transport,
      [](const auto&, std::string request) -> service::RpcHost::Reply {
        if (request == "watch")
          return [sequence = 0]() mutable -> std::optional<service::RpcHost::Message> {
            return service::RpcHost::Message(std::to_string(++sequence), true);
          };
        return [request] { return std::optional<std::string>(request); };
      },
      {}, progress);
  std::jthread worker([&] { static_cast<void>(server.run()); });
  struct Stop {
    ~Stop() { service::request_stop(); }
  } stop;
  ipc::RpcClient client(transport.endpoint, 2, PayloadBudget{128 * 1024 * 1024});
  unsigned frames = 0;
  auto watching = client.watch("watch", 1s,
                               [&](Payload value) { EXPECT_EQ(*value, std::to_string(++frames)); });
  auto control = client.request("control", 1s);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while ((frames < 5 || control.wait_for(0ms) != std::future_status::ready) &&
         std::chrono::steady_clock::now() < deadline) {
    const auto before = frames;
    client.poll();
    EXPECT_LE(frames - before, 1U);
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_GE(frames, 5U);
  ASSERT_EQ(control.wait_for(0ms), std::future_status::ready);
  EXPECT_EQ(*control.get(), "control");
  client.cancel();
  EXPECT_THROW(watching.get(), Error);
  auto invalid = client.watch("watch", 1s, [](Payload) {
    throw std::invalid_argument("fixture rejects the stream frame");
  });
  const auto until = std::chrono::steady_clock::now() + 2s;
  while (invalid.wait_for(0ms) != std::future_status::ready &&
         std::chrono::steady_clock::now() < until) {
    client.poll();
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_EQ(invalid.wait_for(0ms), std::future_status::ready);
  EXPECT_THROW(invalid.get(), std::invalid_argument);
}

TEST(RpcClient, SharedPayloadAllowanceFollowsRepliesBeyondConnectionsAndTheirReaders) {
  service::reset_stop_request();
  service::Transport transport;
  transport.endpoint = "/tmp/ast-payload-" + unique_process_id() + ".sock";
  Progress progress;
  service::RpcHost server(
      transport,
      [](const auto&, std::string) -> service::RpcHost::Reply {
        return [] { return std::optional<std::string>(std::string(48, 'x')); };
      },
      {}, progress);
  std::jthread worker([&] { static_cast<void>(server.run()); });
  struct Stop {
    ~Stop() { service::request_stop(); }
  } stop;
  const auto read = [](ipc::RpcClient& client) {
    auto pending = client.request("read", 1s);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (pending.wait_for(0ms) != std::future_status::ready) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("fixture RPC did not complete");
      client.poll();
      std::this_thread::sleep_for(1ms);
    }
    return pending.get();
  };
  PayloadBudget shared(64);
  Payload held;
  {
    ipc::RpcClient first(transport.endpoint, 1, shared, 64);
    held = read(first);
  }
  ASSERT_EQ(*held, std::string(48, 'x'));
  ipc::RpcClient second(transport.endpoint, 1, shared, 64);
  EXPECT_THROW(second.request(std::string(17, 'q'), 1s), Error);
  try {
    (void)read(second);
    FAIL() << "connection completion released bytes still owned by a reader";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), ErrorCode::resource_exhausted);
    EXPECT_STREQ(error.what(),
                 "RPC response payload capacity reached; command outcome may be unknown");
  }
  // A separate control allowance remains usable while the data reader is full.
  ipc::RpcClient control(transport.endpoint, 1, PayloadBudget{64}, 64);
  EXPECT_EQ(*read(control), *held);
  std::jthread consumer([held = std::move(held)]() mutable { held.reset(); });
  consumer.join();
  EXPECT_EQ(*read(second), std::string(48, 'x'));
}

TEST(RpcClient, MutualTlsRequiresTrustedClientAndMatchingServerIdentity) {
  const auto root =
      std::filesystem::temp_directory_path() / ("ast-client-tls-" + unique_process_id());
  std::filesystem::create_directory(root);
  struct Remove {
    std::filesystem::path root;
    ~Remove() { std::filesystem::remove_all(root); }
  } remove{root};
  ChildProcess certificates(ASTERION_CERTIFICATES, {root.string()});
  ASSERT_TRUE(certificates.wait(5s));
  ASSERT_EQ(certificates.exit_code(), 0);
  for (const auto& [server_name, client_name, succeeds] : {std::tuple{"server", "client", true},
                                                           {"server", "stranger", false},
                                                           {"wrong", "client", false}}) {
    service::reset_stop_request();
    asio::io_context io;
    asio::ip::tcp::acceptor reserve(io, {asio::ip::make_address("127.0.0.1"), 0});
    service::Transport transport;
    transport.bind = "127.0.0.1";
    transport.port = reserve.local_endpoint().port();
    reserve.close();
    const auto identity = [&](std::string name) {
      return ipc::TlsIdentity{(root / "ca.crt").string(), (root / (name + ".crt")).string(),
                              (root / (name + ".key")).string()};
    };
    transport.tls = identity(server_name);
    Progress progress;
    service::RpcHost server(
        transport,
        [](const auto&, std::string request) -> service::RpcHost::Reply {
          return [request = std::move(request)] { return std::optional<std::string>(request); };
        },
        {}, progress);
    std::jthread worker([&] { static_cast<void>(server.run()); });
    struct Stop {
      ~Stop() { service::request_stop(); }
    } stop;
    ipc::RpcClient client("127.0.0.1", transport.port, identity(client_name), 1,
                          PayloadBudget{128 * 1024 * 1024});
    auto response = client.request("authenticated", 1s);
    const auto deadline = std::chrono::steady_clock::now() + 6s;
    while (response.wait_for(0ms) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
      client.poll();
      std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(response.wait_for(0ms), std::future_status::ready);
    if (succeeds)
      EXPECT_EQ(*response.get(), "authenticated");
    else
      EXPECT_THROW(static_cast<void>(response.get()), Error);
  }
}
