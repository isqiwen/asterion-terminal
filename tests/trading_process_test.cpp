#include <gtest/gtest.h>
#include <asterion/protocol/trading.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
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
        directory = std::filesystem::temp_directory_path() / ("asterion-process-" + unique_process_id());
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
        process = std::make_unique<ChildProcess>(executable, std::vector<std::string>{"--mode", "paper", "--session", id, "--endpoint", endpoint, "--directory", std::string(path.begin(), path.end())});
        const auto end = std::chrono::steady_clock::now() + 3s;
        for (;;) {
            try { channel = ipc::Channel::connect(endpoint, 200ms); break; }
            catch (const Error&) { if (process->exited() || std::chrono::steady_clock::now() >= end) throw; std::this_thread::sleep_for(10ms); }
        }
    }
    ~Host() { channel.close(); process.reset(); std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    wire::Request request() { wire::Request r; r.set_version(1); r.set_session_id(id); r.set_mode(wire::PAPER); r.set_correlation_id("test.request"); return r; }
    wire::Response call(const wire::Request& r) {
        channel.send(r.SerializeAsString(), 2s); wire::Response response;
        if (!response.ParseFromString(channel.receive(2s))) throw std::runtime_error("bad response");
        EXPECT_EQ(response.session_id(), id); EXPECT_EQ(response.mode(), wire::PAPER); EXPECT_EQ(response.correlation_id(), r.correlation_id()); return response;
    }
    wire::Request create(std::string deposit) {
        auto r = request();
        Json input{{"version", 1}, {"type", "historical_paper"}, {"risk", {{"max_order_quantity","100"}, {"max_gross_quantity","100"}, {"max_working_orders",std::uint64_t{100}}}},  {"deposit", deposit},
            {"contract", {{"venue", "SHFE"}, {"symbol", "rb2610"}, {"currency", "CNY"}, {"price_increment", "1"}, {"quantity_increment", "1"}, {"multiplier", "10"}, {"product", "rb"}, {"delivery_month", "2026-10"}}},
            {"costs", {{"margin_per_lot", "100"}, {"open_fee", "2"}, {"close_today_fee", "3"}, {"close_yesterday_fee", "4"}}},
            {"ticks", Json::array({{{"timestamp_ns", "100"}, {"price", "100"}, {"quantity", "1"}}, {{"timestamp_ns", "200"}, {"price", "101"}, {"quantity", "1"}}})}};
        *r.mutable_create() = protocol::encode_input(input); return r;
    }
};
}
TEST(TradingProcess, RejectsVersionModeAndSessionMismatchBeforeJournalWrite) {
    Host host("paper.identity");
    auto r = host.create("1000"); r.set_version(2); EXPECT_TRUE(host.call(r).has_error());
    r.set_version(1); r.set_mode(wire::LIVE); EXPECT_TRUE(host.call(r).has_error());
    r.set_mode(wire::PAPER); r.set_session_id("other"); EXPECT_TRUE(host.call(r).has_error());
    EXPECT_TRUE(std::filesystem::is_empty(host.directory));
    r = host.create("1000"); EXPECT_TRUE(host.call(r).has_snapshot());
    EXPECT_TRUE(host.call(r).has_error()); // Creating again cannot replace the ledger.
    r = host.request(); r.mutable_shutdown(); EXPECT_TRUE(host.call(r).has_error()); EXPECT_FALSE(host.process->exited());
}
TEST(TradingProcess, IndependentInstancesHaveSeparateLedgersAndClocks) {
    Host a("paper.a"), b("paper.b"); EXPECT_NE(a.process->id(), b.process->id());
    ASSERT_TRUE(a.call(a.create("1000")).has_snapshot()); ASSERT_TRUE(b.call(b.create("2000")).has_snapshot());
    auto advance = a.request(); advance.mutable_command()->set_request_id("tick1"); advance.mutable_command()->mutable_advance();
    auto left = a.call(advance); ASSERT_TRUE(left.has_snapshot()); EXPECT_EQ(left.snapshot().cursor(), 1);
    auto query = b.request(); query.mutable_snapshot(); auto right = b.call(query); ASSERT_TRUE(right.has_snapshot());
    EXPECT_EQ(right.snapshot().cursor(), 0); EXPECT_EQ(right.snapshot().balance().units(), 200000000000LL);
    EXPECT_EQ(left.snapshot().balance().units(), 100000000000LL);
}
