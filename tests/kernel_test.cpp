#include <asterion/kernel/process/file_lock.hpp>
#ifndef _WIN32
#include <sys/wait.h>
#include <sys/stat.h>
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
#include <asterion/kernel/thread_pool.hpp>
#include <asterion/kernel/polled_task.hpp>
#include <condition_variable>
#include <future>
#include <barrier>
#include <asterion/kernel/process/child.hpp>
#include <iostream>
using namespace asterion;
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
  EXPECT_THROW(FileLock(root, "missing.lock", Access::shared_existing), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(root / "missing.lock"));
  std::filesystem::create_directory(root / "directory.lock");
  EXPECT_THROW(FileLock(root, "directory.lock", Access::shared_existing), std::runtime_error);
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
    EXPECT_NO_THROW(FileLock(root, "dataset.lock", Access::shared_existing));
    EXPECT_NO_THROW(FileLock(root, "dataset.lock", Access::shared));
    EXPECT_THROW(FileLock(root, "dataset.lock"), std::runtime_error);
#ifndef _WIN32
    peer(Access::shared, true);
    peer(Access::exclusive, false);
#endif
  }
  {
    FileLock writer(root, "dataset.lock");
    EXPECT_THROW(FileLock(root, "dataset.lock", Access::shared_existing), std::runtime_error);
    EXPECT_THROW(FileLock(root, "dataset.lock", Access::shared), std::runtime_error);
    EXPECT_THROW(FileLock(root, "dataset.lock"), std::runtime_error);
#ifndef _WIN32
    peer(Access::shared, false);
    peer(Access::exclusive, false);
#endif
  }
  EXPECT_NO_THROW(FileLock(root, "dataset.lock"));
}

#ifndef _WIN32
namespace {
class DurableFiles : public testing::Test {
protected:
  std::filesystem::path root;
  void SetUp() override {
    root = std::filesystem::temp_directory_path() / ("ast-durable-" + unique_process_id());
    std::filesystem::create_directory(root);
  }
  void TearDown() override {
    fail_next_directory_syncs_for_testing(0);
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  static std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
      throw std::runtime_error("fixture file is missing");
    return {std::istreambuf_iterator<char>(file), {}};
  }
};
} // namespace
TEST_F(DurableFiles, LinksAndSpecialFilesAreRejectedBeforeChangingTargetBytesOrPermissions) {
  const auto original = root / "original";
  write_file_durably(original, "preserved");
  using std::filesystem::perms;
  std::filesystem::permissions(original,
                               perms::owner_read | perms::owner_write | perms::group_read);
  const auto mode = std::filesystem::status(original).permissions();
  std::filesystem::create_symlink(original, root / "symbolic");
  EXPECT_THROW(write_file_durably(root / "symbolic", "wrong"), std::runtime_error);
  std::filesystem::create_hard_link(original, root / "hard");
  EXPECT_THROW(write_file_durably(root / "hard", "wrong"), std::runtime_error);
  EXPECT_EQ(read(original), "preserved");
  EXPECT_EQ(std::filesystem::status(original).permissions(), mode);
  std::filesystem::create_symlink(root / "not-created", root / "dangling");
  EXPECT_THROW(write_file_durably(root / "dangling", "wrong"), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(root / "not-created"));
  ASSERT_EQ(::mkfifo((root / "fifo").c_str(), 0600), 0);
  EXPECT_THROW(write_file_durably(root / "fifo", "wrong"), std::runtime_error);
  EXPECT_THROW(publish_file_durably(root / "fifo", root / "published"), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(root / "published"));
}
TEST_F(DurableFiles, ExternalFileSyncPreservesIdentityAndRejectsUnsafeObjects) {
  const auto file = root / "external-key";
  std::ofstream(file) << "externally-generated-test-key";
  std::filesystem::permissions(file, std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write);
  struct stat before{};
  ASSERT_EQ(::lstat(file.c_str(), &before), 0);
  const auto modified = std::filesystem::last_write_time(file).time_since_epoch().count();
  fail_next_directory_syncs_for_testing(1);
  EXPECT_THROW(sync_file_durably(file), std::runtime_error);
  EXPECT_NO_THROW(sync_file_durably(file));
  struct stat after{};
  ASSERT_EQ(::lstat(file.c_str(), &after), 0);
  EXPECT_EQ(before.st_ino, after.st_ino);
  EXPECT_EQ(before.st_mode, after.st_mode);
  EXPECT_EQ(std::filesystem::last_write_time(file).time_since_epoch().count(), modified);
  EXPECT_EQ(read(file), "externally-generated-test-key");
  std::filesystem::create_symlink(file, root / "symbolic");
  EXPECT_THROW(sync_file_durably(root / "symbolic"), std::runtime_error);
  std::filesystem::create_hard_link(file, root / "hard");
  EXPECT_THROW(sync_file_durably(root / "hard"), std::runtime_error);
  ASSERT_EQ(::mkfifo((root / "fifo").c_str(), 0600), 0);
  EXPECT_THROW(sync_file_durably(root / "fifo"), std::runtime_error);
  EXPECT_THROW(sync_file_durably(root / "missing"), std::runtime_error);
  EXPECT_EQ(read(file), "externally-generated-test-key");
}
TEST_F(DurableFiles, UniqueReplacementPreservesUnrelatedTemporaryFilesAndCleansFailures) {
  const auto file = root / "state";
  write_file_durably(file, "old");
  write_file_durably(root / "state.tmp", "user-owned-pending-data");
  replace_file_durably(file, "new");
  EXPECT_EQ(read(file), "new");
  EXPECT_EQ(read(root / "state.tmp"), "user-owned-pending-data");
  fail_next_directory_syncs_for_testing(1);
  EXPECT_THROW(replace_file_durably(file, "published-before-sync-error"), std::runtime_error);
  fail_next_directory_syncs_for_testing(0);
  EXPECT_EQ(read(file), "published-before-sync-error");
  std::filesystem::create_directory(root / "directory-target");
  EXPECT_THROW(replace_file_durably(root / "directory-target", "wrong"),
               std::filesystem::filesystem_error);
  for (const auto& entry : std::filesystem::directory_iterator(root))
    EXPECT_TRUE(entry.path().filename() == "state" || entry.path().filename() == "state.tmp" ||
                entry.path().filename() == "directory-target");
}
TEST_F(DurableFiles, ConcurrentReplacementsPublishOnlyCompletePayloads) {
  const auto file = root / "shared";
  constexpr std::size_t count = 32768;
  write_file_durably(file, std::string(count, 'A'));
  std::atomic<bool> corrupted{false};
  std::jthread reader([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      try {
        const auto bytes = read(file);
        if (bytes.size() != count || bytes.front() < 'A' || bytes.front() > 'D' ||
            bytes.find_first_not_of(bytes.front()) != std::string::npos)
          corrupted = true;
      } catch (...) {
        corrupted = true;
        break;
      }
    }
  });
  std::barrier begin(4);
  std::vector<std::future<void>> writers;
  for (int writer = 0; writer < 4; ++writer)
    writers.push_back(std::async(std::launch::async, [&, writer] {
      const std::string bytes(count, static_cast<char>('A' + writer));
      begin.arrive_and_wait();
      for (int i = 0; i < 8; ++i)
        replace_file_durably(file, bytes);
    }));
  for (auto& writer : writers)
    EXPECT_NO_THROW(writer.get());
  reader.request_stop();
  reader.join();
  EXPECT_FALSE(corrupted);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(root),
                          std::filesystem::directory_iterator()),
            1);
}
TEST_F(DurableFiles, PublicationChecksTheOpenedSourceAndSyncsBothDirectories) {
  std::filesystem::create_directory(root / "incoming");
  std::filesystem::create_directory(root / "published");
  const auto source = root / "incoming/payload";
  const auto target = root / "published/payload";
  write_file_durably(source, "complete");
  std::filesystem::create_symlink(source, root / "alias");
  EXPECT_THROW(publish_file_durably(root / "alias", target), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(target));
  publish_file_durably(source, target);
  EXPECT_EQ(read(target), "complete");
  EXPECT_FALSE(std::filesystem::exists(source));
}
#endif

TEST(Kernel, PolledOperationsApplyResultsOnlyOnTheOwnerAndPropagateFailure) {
  std::promise<int> completion;
  int observed = 0;
  auto nested = [&](std::future<int> future) -> PolledTask<int> {
    co_return co_await PollFuture{std::move(future)};
  };
  auto apply = [&](std::future<int> future) -> PolledTask<> {
    observed = co_await nested(std::move(future));
  };
  auto waiting = apply(completion.get_future());
  EXPECT_FALSE(waiting.poll());
  ThreadPool worker(1, 1);
  worker.submit([&](std::stop_token) { completion.set_value(7); }).get();
  EXPECT_EQ(observed, 0);
  std::promise<int> other;
  other.set_value(3);
  auto available = apply(other.get_future());
  ASSERT_TRUE(available.poll());
  available.take();
  EXPECT_EQ(observed, 3);
  ASSERT_TRUE(waiting.poll());
  waiting.take();
  EXPECT_EQ(observed, 7);

  std::promise<int> failed;
  auto rejected = apply(failed.get_future());
  EXPECT_FALSE(rejected.poll());
  failed.set_exception(std::make_exception_ptr(std::runtime_error("file failure")));
  ASSERT_TRUE(rejected.poll());
  EXPECT_THROW(rejected.take(), std::runtime_error);
  EXPECT_EQ(observed, 7);
}
