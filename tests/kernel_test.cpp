#include <asterion/kernel/process/file_lock.hpp>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <asterion/kernel/environment.hpp>
#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <asterion/kernel/durable_file.hpp>
#include <gtest/gtest.h>
#include <asterion/kernel/runtime.hpp>
#include <condition_variable>
#include <future>
#include <iostream>
using namespace asterion;
TEST(Kernel, configuration_and_resources) {
  Configuration config;
  config.declare("plugin.limit", 10, [](const Json& value) {
    return value.is_number_integer() && value > 0 && value <= 100;
  });
  EXPECT_THROW(([&] { config.apply({{"plugin.limit", 20}, {"unexpected", 1}}); })(), Error);
  EXPECT_TRUE((config.at("plugin.limit") == 10)) << "configuration rejection is atomic";
  EXPECT_THROW(([&] { config.apply({{"plugin.limit", "20"}}); })(), Error);
  const auto saved = config.at("plugin.limit");
  config.apply({{"plugin.limit", 20}});
  config.seal();
  EXPECT_TRUE((saved == 10)) << "configuration reads are immutable snapshots";
  EXPECT_THROW(([&] { config.apply({{"plugin.limit", 30}}); })(), Error);
  ResourceRegistry resources(1, 1);
  auto scope = resources.create_scope("plugin.one");
  scope.publish("counter", std::make_shared<int>(42));
  auto handle = resources.resolve<int>("plugin.one", "counter");
  auto retained = handle.lock();
  EXPECT_THROW(([&] { scope.publish("counter", std::make_shared<int>(1)); })(), Error);
  EXPECT_THROW(([&] { scope.publish("second", std::make_shared<int>(1)); })(), Error);
  EXPECT_THROW(
      ([&] { static_cast<void>(resources.resolve<std::string>("plugin.one", "counter")); })(),
      Error);
  EXPECT_THROW(([&] { static_cast<void>(resources.create_scope("plugin.two")); })(), Error);
  scope.close();
  EXPECT_THROW(([&] { static_cast<void>(handle.lock()); })(), Error);
  EXPECT_TRUE((*retained == 42)) << "revocation preserves memory lifetime of existing borrowers";
  auto replacement = resources.create_scope("plugin.one");
  replacement.publish("counter", std::make_shared<int>(99));
  EXPECT_THROW(([&] { static_cast<void>(handle.lock()); })(), Error);
  EXPECT_TRUE((*resources.resolve<int>("plugin.one", "counter").lock() == 99))
      << "new scope does not revive old handle";
  resources.clear();
  EXPECT_THROW(([&] { replacement.publish("new", std::make_shared<int>(0)); })(), Error);
}
TEST(Kernel, resource_destructor_reentry) {
  ResourceRegistry resources;
  auto scope = resources.create_scope("owner");
  struct Cleanup {
    std::function<void()> callback;
    ~Cleanup() { callback(); }
  };
  bool cleaned = false;
  auto cleanup = std::make_shared<Cleanup>();
  cleanup->callback = [&] {
    EXPECT_THROW(([&] { static_cast<void>(resources.resolve<int>("owner", "value")); })(), Error);
    cleaned = true;
  };
  scope.publish("cleanup", std::move(cleanup));
  resources.clear();
  EXPECT_TRUE((cleaned)) << "resource destructors may reenter registry without deadlock";
}
TEST(Kernel, scheduling_and_messages) {
  ManualClock clock;
  Scheduler scheduler(clock, 4);
  std::vector<int> order;
  Scheduler::TaskId cancelled = 0;
  scheduler.after(10, [&] {
    order.push_back(1);
    scheduler.cancel(cancelled);
    scheduler.after(0, [&] { order.push_back(4); });
  });
  cancelled = scheduler.after(10, [&] { order.push_back(99); });
  scheduler.after(10, [&] {
    order.push_back(2);
    throw std::runtime_error("task failed");
  });
  scheduler.after(10, [&] { order.push_back(3); });
  EXPECT_THROW(([&] { scheduler.after(0, [] {}); })(), Error);
  EXPECT_TRUE((scheduler.run_due() == 0)) << "future tasks not run early";
  clock.advance(10);
  EXPECT_THROW(([&] { scheduler.run_due(); })(), std::runtime_error);
  EXPECT_TRUE((order == std::vector<int>({1, 2, 3})))
      << "stable order, cancellation, exception isolation";
  EXPECT_TRUE((scheduler.run_due() == 1 && order.back() == 4))
      << "reentrant scheduling deferred to next pump";
  scheduler.after(0, [&] { scheduler.run_due(); });
  EXPECT_THROW(([&] { scheduler.run_due(); })(), Error);
  scheduler.close();
  EXPECT_THROW(([&] { scheduler.after(0, [] {}); })(), Error);
  MessageBus<int> bus(2);
  int received = 0;
  auto first = bus.subscribe([&](int value) {
    if (value == 1) {
      EXPECT_TRUE((bus.post(3))) << "callback post";
      throw std::runtime_error("subscriber failure");
    }
  });
  bus.subscribe([&](int value) { received += value; });
  EXPECT_TRUE((bus.post(1) && bus.post(2) && !bus.post(99))) << "bus backpressure explicit";
  EXPECT_THROW(([&] { bus.dispatch(); })(), std::runtime_error);
  EXPECT_TRUE((received == 3)) << "all subscribers and accepted batch delivered despite failure";
  bus.unsubscribe(first);
  EXPECT_TRUE((bus.dispatch() == 1 && received == 6))
      << "callback post delivered next dispatch without replay";
  bus.close();
  EXPECT_TRUE((!bus.post(4))) << "closed message bus rejects producers";
  MessageBus<int> closing(2);
  int finished = 0;
  closing.subscribe([&](int) { closing.close(); });
  closing.subscribe([&](int) { ++finished; });
  closing.post(1);
  closing.post(2);
  EXPECT_TRUE((closing.dispatch() == 1 && finished == 1))
      << "close completes current subscription snapshot but drops remaining batch";
}
TEST(Kernel, worker_shutdown) {
  ThreadPool worker(1, 1);
  std::promise<void> entered;
  auto running = worker.submit([&](std::stop_token stop) {
    entered.set_value();
    std::mutex mutex;
    std::condition_variable_any condition;
    std::unique_lock lock(mutex);
    condition.wait(lock, stop, [] { return false; });
  });
  entered.get_future().wait();
  bool executed = false;
  auto pending = worker.submit([&](std::stop_token) { executed = true; });
  EXPECT_THROW(([&] { static_cast<void>(worker.submit([](std::stop_token) {})); })(), Error);
  worker.shutdown();
  running.get();
  EXPECT_THROW(([&] { pending.get(); })(), Error);
  EXPECT_TRUE((!executed)) << "shutdown cancels pending tasks before plugin teardown";
  EXPECT_THROW(([&] { static_cast<void>(worker.submit([](std::stop_token) {})); })(), Error);
  ThreadPool isolated;
  auto failed = isolated.submit([](std::stop_token) { throw std::runtime_error("failure"); });
  auto next = isolated.submit([](std::stop_token) {});
  EXPECT_THROW(([&] { failed.get(); })(), std::runtime_error);
  next.get();
}
struct LifecyclePlugin final : Plugin {
  std::vector<std::string>& events;
  std::string name;
  bool fail;
  LifecyclePlugin(std::vector<std::string>& events_, std::string name_, bool fail_ = false)
      : events(events_), name(std::move(name_)), fail(fail_) {}
  PluginDescriptor descriptor() const override {
    return {name, PluginKind::tool, plugin_contract_version, {}};
  }
  void start() override {
    events.push_back(name + ".start");
    if (fail)
      throw std::runtime_error("start failure");
  }
  void stop() noexcept override { events.push_back(name + ".stop"); }
};
TEST(Kernel, runtime_integration) {
  auto clock = std::make_shared<ManualClock>(123);
  Runtime runtime("test.runtime", clock);
  std::vector<std::string> lifecycle;
  runtime.add_plugin(std::make_unique<LifecyclePlugin>(lifecycle, "first"));
  runtime.add_plugin(std::make_unique<LifecyclePlugin>(lifecycle, "second"));
  auto scope = runtime.resources().create_scope("service");
  scope.publish("value", std::make_shared<int>(7));
  auto handle = runtime.resources().resolve<int>("service", "value");
  runtime.access().grant("test.user", "service.read");
  runtime.command("service.read", "service.read", [&](const Json&) {
    clock->advance(5);
    return Json(*handle.lock());
  });
  runtime.command("service.stop", "service.read", [&](const Json&) {
    runtime.stop();
    return Json();
  });
  EXPECT_THROW(([&] { runtime.dispatch("test.user", "service.read", {}); })(), Error);
  runtime.start();
  EXPECT_TRUE((runtime.dispatch("test.user", "service.read", {}) == 7))
      << "runtime command resolves owned resource";
  EXPECT_THROW(([&] { runtime.dispatch("intruder", "service.read", {}); })(), Error);
  EXPECT_THROW(([&] { runtime.dispatch("test.user", "service.stop", {}); })(), Error);
  EXPECT_TRUE((runtime.state() == RuntimeState::running))
      << "callback cannot tear down running command";
  EXPECT_TRUE((runtime.observations().metrics().succeeded == 1 &&
               runtime.observations().metrics().failed == 2))
      << "runtime traces successes and denied/failed commands";
  EXPECT_TRUE((runtime.observations().recent().front().duration_ns == 5))
      << "monotonic trace timing";
  bool delivered = false;
  runtime.messages().subscribe([&](const EventEnvelope&) { delivered = true; });
  runtime.scheduler().after(0, [&] { throw std::runtime_error("scheduled failure"); });
  runtime.thread_pool()
      .submit([&](std::stop_token) {
        EXPECT_TRUE(
            (runtime.messages().post({"worker:1", "worker", "completed", 123, Json::object()})))
            << "worker sends event";
      })
      .get();
  EXPECT_THROW(([&] { runtime.poll(); })(), std::runtime_error);
  EXPECT_TRUE((delivered)) << "runtime pump delivers worker events despite timer failure";
  runtime.access().revoke("test.user");
  EXPECT_THROW(([&] { runtime.dispatch("test.user", "service.read", {}); })(), Error);
  EXPECT_THROW(([&] { runtime.access().grant("intruder", "service.read"); })(), Error);
  runtime.stop();
  EXPECT_TRUE((lifecycle == std::vector<std::string>(
                                {"first.start", "second.start", "second.stop", "first.stop"})))
      << "runtime owns reverse teardown";
  EXPECT_THROW(([&] { static_cast<void>(handle.lock()); })(), Error);
  EXPECT_THROW(([&] { runtime.start(); })(), Error);
  Observability bounded(2);
  bounded.record({"t:1", "request", 1, 0, true});
  bounded.record({"t:2", "request", 2, 1, false});
  bounded.record({"t:3", "request", 3, 2, true});
  EXPECT_TRUE(
      (bounded.recent().size() == 2 && bounded.dropped() == 1 && bounded.metrics().succeeded == 2))
      << "bounded traces and cumulative metrics";
  std::vector<std::string> failed_lifecycle;
  Runtime failed("test.failed");
  auto failed_scope = failed.resources().create_scope("failed");
  failed_scope.publish("value", std::make_shared<int>(1));
  auto failed_handle = failed.resources().resolve<int>("failed", "value");
  failed.add_plugin(std::make_unique<LifecyclePlugin>(failed_lifecycle, "first"));
  failed.add_plugin(std::make_unique<LifecyclePlugin>(failed_lifecycle, "second", true));
  EXPECT_THROW(([&] { failed.start(); })(), std::runtime_error);
  EXPECT_TRUE((failed.state() == RuntimeState::failed && failed_lifecycle.back() == "first.stop"))
      << "startup rollback exposes failed state";
  EXPECT_THROW(([&] { static_cast<void>(failed_handle.lock()); })(), Error);
}

TEST(ThreadPool, WorkersRunConcurrentlyAndShutdownCancelsQueue) {
  std::mutex mutex;
  std::condition_variable_any condition;
  int arrived = 0;
  ThreadPool pool(3, 8);
  std::vector<std::future<void>> active;
  for (int i = 0; i < 3; ++i)
    active.push_back(pool.submit([&](std::stop_token stop) {
      std::unique_lock lock(mutex);
      ++arrived;
      condition.notify_all();
      condition.wait(lock, stop, [] { return false; });
    }));
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(3), [&] { return arrived == 3; }));
  }
  std::atomic<int> ran = 0;
  std::vector<std::future<void>> pending;
  for (int i = 0; i < 8; ++i)
    pending.push_back(pool.submit([&](std::stop_token) { ++ran; }));
  EXPECT_THROW(pool.submit([](std::stop_token) {}), Error);
  pool.shutdown();
  for (auto& task : active)
    EXPECT_NO_THROW(task.get());
  for (auto& task : pending)
    EXPECT_THROW(task.get(), Error);
  EXPECT_EQ(ran.load(), 0);
  EXPECT_NO_THROW(pool.shutdown());
}

TEST(ThreadPool, InvalidSizeAndWorkerSelfJoinAreRejected) {
  EXPECT_THROW(ThreadPool(0), Error);
  EXPECT_THROW(ThreadPool(257), Error);
  ThreadPool pool(1);
  auto self = pool.submit([&](std::stop_token) { pool.shutdown(); });
  EXPECT_THROW(self.get(), Error);
  auto next = pool.submit([](std::stop_token) {});
  EXPECT_NO_THROW(next.get());
}
TEST(Kernel, DurableFilesAreOwnerOnlyAndReplaceAtomically) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("asterion-durable-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(root);
  const auto file = root / "secret.pem";
  write_file_durably(file, "first");
  {
    // Closed before the replacement: Windows cannot replace an open file.
    std::ifstream first(file, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(first), {}), "first");
  }
#ifndef _WIN32
  using std::filesystem::perms;
  EXPECT_EQ(std::filesystem::status(file).permissions() & perms::all,
            perms::owner_read | perms::owner_write);
#endif
  replace_file_durably(file, "second");
  {
    std::ifstream second(file, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(second), {}), "second");
  }
  EXPECT_FALSE(std::filesystem::exists(root / "secret.pem.tmp"));
#ifndef _WIN32
  std::filesystem::create_symlink(file, root / "link");
  EXPECT_THROW(replace_file_durably(root / "link", "x"), std::runtime_error);
#endif
  std::filesystem::remove_all(root);
}
TEST(Kernel, EnvironmentLookupsAreUnicodeAndTreatEmptyAsUnset) {
  const char* name = "ASTERION_TEST_ENVIRONMENT_PATH";
  const std::string value = "/tmp/用户/数据";
#ifdef _WIN32
  const std::wstring key(name, name + std::strlen(name));
  const std::filesystem::path expected(std::u8string(value.begin(), value.end()));
  ASSERT_TRUE(SetEnvironmentVariableW(key.c_str(), expected.wstring().c_str()));
#else
  ASSERT_EQ(::setenv(name, value.c_str(), 1), 0);
#endif
  EXPECT_EQ(environment_variable(name), value);
  EXPECT_EQ(environment_path(name),
            std::filesystem::path(std::u8string(value.begin(), value.end())));
#ifdef _WIN32
  SetEnvironmentVariableW(key.c_str(), L"");
#else
  ::setenv(name, "", 1);
#endif
  EXPECT_FALSE(environment_variable(name).has_value());
  EXPECT_FALSE(environment_path("ASTERION_TEST_ENVIRONMENT_UNSET").has_value());
}
#ifndef _WIN32
#include <asterion/kernel/process/child.hpp>
#include <fcntl.h>
#include <unistd.h>
TEST(Kernel, child_does_not_inherit_host_pipes) {
  for (bool independent : {false, true}) {
    int pipe_fds[2];
    ASSERT_EQ(::pipe(pipe_fds), 0);
    struct Descriptors {
      int read, write;
      ~Descriptors() {
        ::close(read);
        if (write >= 0)
          ::close(write);
      }
    } descriptors{pipe_fds[0], pipe_fds[1]};
    ASSERT_EQ(::fcntl(descriptors.read, F_SETFL, O_NONBLOCK), 0);
    // Deliberately inheritable: this models Electron's extra protocol pipes.
    ASSERT_EQ(::fcntl(descriptors.write, F_SETFD, 0), 0);
    ChildProcess child("/bin/sleep", {"2"}, independent);
    ::close(descriptors.write);
    descriptors.write = -1;
    ASSERT_FALSE(child.exited());
    char byte;
    EXPECT_EQ(::read(descriptors.read, &byte, 1), 0)
        << "A running child kept the host's pipe writer alive";
  }
}
#endif

TEST(Kernel, FileLockReadersShareWhileWritersRemainExclusive) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("asterion-shared-lock-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(root);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{root};
  using Access = FileLock::Access;
#ifndef _WIN32
  const auto peer = [&](Access access, bool expected) {
    const auto pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      bool acquired = false;
      try {
        FileLock lock(root, "dataset.lock", access);
        acquired = true;
      } catch (...) {
      }
      _exit(acquired == expected ? 0 : 1);
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
  };
#endif
  {
    FileLock reader(root, "dataset.lock", Access::shared);
    EXPECT_NO_THROW(FileLock(root, "dataset.lock", Access::shared));
    EXPECT_THROW(FileLock(root, "dataset.lock"), std::runtime_error);
#ifndef _WIN32
    peer(Access::shared, true);
    peer(Access::exclusive, false);
#endif
  }
  {
    FileLock writer(root, "dataset.lock");
    EXPECT_THROW(FileLock(root, "dataset.lock", Access::shared), std::runtime_error);
    EXPECT_THROW(FileLock(root, "dataset.lock"), std::runtime_error);
#ifndef _WIN32
    peer(Access::shared, false);
    peer(Access::exclusive, false);
#endif
  }
  EXPECT_NO_THROW(FileLock(root, "dataset.lock"));
}
