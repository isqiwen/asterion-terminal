#include <gtest/gtest.h>
#include <asterion/protocol/trading.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <thread>
using namespace asterion;
using namespace std::chrono_literals;
namespace wire = asterion::protocol::v1;
namespace {
struct Host {
  std::filesystem::path directory;
  std::string endpoint, id;
  std::unique_ptr<ChildProcess> process;
  ipc::Channel channel;
  explicit Host(std::string session) : id(std::move(session)) {
    directory =
        std::filesystem::temp_directory_path() / ("asterion-process-" + unique_process_id());
    std::filesystem::create_directory(directory);
#ifdef _WIN32
    endpoint = "asterion.test." + unique_process_id();
    const auto executable = current_executable().parent_path() / "asterion-trading.exe";
#else
    // macOS's temp_directory_path can exceed sockaddr_un's path length.
    endpoint = "/tmp/ast-" + unique_process_id() + ".sock";
    const auto executable = current_executable().parent_path() / "asterion-trading";
#endif
    const auto path = directory.u8string();
    process = std::make_unique<ChildProcess>(
        executable, std::vector<std::string>{"--session", id, "--endpoint", endpoint, "--directory",
                                             std::string(path.begin(), path.end()), "--ctp-library",
                                             ASTERION_TEST_CTP_TRADER});
    const auto end = std::chrono::steady_clock::now() + 3s;
    for (;;) {
      try {
        channel = ipc::Channel::connect(endpoint, 200ms);
        break;
      } catch (const Error&) {
        if (process->exited() || std::chrono::steady_clock::now() >= end)
          throw;
        std::this_thread::sleep_for(10ms);
      }
    }
  }
  ~Host() {
    channel.close();
    process.reset();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  wire::Request request() {
    wire::Request r;
    r.set_version(1);
    r.set_session_id(id);
    r.set_correlation_id("test.request");
    return r;
  }
  wire::Response call(const wire::Request& r) {
    channel.send(r.SerializeAsString(), 2s);
    wire::Response response;
    if (!response.ParseFromString(channel.receive(2s)))
      throw std::runtime_error("bad response");
    EXPECT_EQ(response.session_id(), id);
    EXPECT_EQ(response.correlation_id(), r.correlation_id());
    return response;
  }
};
} // namespace
TEST(TradingProcess, RejectsVersionAndSessionMismatchBeforeWritingARecord) {
  Host host("live.identity");
  auto r = host.request();
  r.mutable_attach();
  r.set_version(2);
  EXPECT_TRUE(host.call(r).has_error());
  r.set_version(1);
  r.set_session_id("other");
  EXPECT_TRUE(host.call(r).has_error());
  r.set_session_id(host.id);
  EXPECT_TRUE(host.call(r).has_uninitialized());
  // Nothing was created: only the service's own log directory may exist.
  EXPECT_FALSE(std::filesystem::exists(host.directory / "journal.sqlite"));
  r = host.request();
  r.mutable_shutdown();
  EXPECT_TRUE(host.call(r).has_error());
  EXPECT_FALSE(host.process->exited());
}
