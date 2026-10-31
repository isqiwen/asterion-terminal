#include "../apps/clients/terminal/native/market_history.hpp"
#include <gtest/gtest.h>
#include <asterion/terminal.h>
#include <nlohmann/json.hpp>
#include <asterion/system/process/child.hpp>
#include <memory>
#include <thread>

using nlohmann::json;

namespace {

json call(void* runtime, const json& request)
{
    std::unique_ptr<char, decltype(&asterion_terminal_free)> result(
        asterion_terminal_call(runtime, request.dump().c_str()),
        asterion_terminal_free);
    EXPECT_TRUE((result != nullptr)) << "response allocated";
    return json::parse(result.get());
}

json request(std::string method, json params = json::object())
{
    return {{"version", 1}, {"method", method}, {"params", params}};
}

} // namespace

TEST(TerminalApi, CreationPreservesTypedFailureAndClearsTheOutput)
{
    const char* value = std::getenv("ASTERION_ROOT");
    const std::optional<std::string> saved =
        value ? std::optional<std::string>(value) : std::nullopt;
    ASSERT_EQ(::setenv("ASTERION_ROOT", "relative", 1), 0);
    char* failure = nullptr;
    std::unique_ptr<void, decltype(&asterion_terminal_destroy)> rejected(
        asterion_terminal_create(&failure),
        asterion_terminal_destroy);
    std::unique_ptr<char, decltype(&asterion_terminal_free)> detail(
        failure,
        asterion_terminal_free);
    if (saved)
        ::setenv("ASTERION_ROOT", saved->c_str(), 1);
    else
        ::unsetenv("ASTERION_ROOT");
    ASSERT_EQ(rejected, nullptr);
    ASSERT_NE(detail, nullptr);
    const auto response = json::parse(detail.get());
    EXPECT_EQ(response.at("error").at("code"), "invalid_request");
    EXPECT_EQ(response.at("error").at("message"), "invalid Asterion root directory");
    failure = detail.get(); // The successful call must clear, not retain, this pointer.
    std::unique_ptr<void, decltype(&asterion_terminal_destroy)> runtime(
        asterion_terminal_create(&failure),
        asterion_terminal_destroy);
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(failure, nullptr);
}

TEST(TerminalApi, SnapshotRejectsMalformedQueriesBeforeReadingState)
{
    std::unique_ptr<void, decltype(&asterion_terminal_destroy)> runtime(
        asterion_terminal_create(nullptr),
        asterion_terminal_destroy);
    ASSERT_NE(runtime, nullptr);
    for (const auto& params : std::vector<json>{
             nullptr,
             json::array(),
             true,
             "invalid",
             {{"extra", true}},
             {{"since", -1}},
             {{"since", 1.5}},
             {{"since", "1"}},
             {{"since", 0}, {"extra", true}}
         }) {
        const auto response = call(runtime.get(), request("runtime.snapshot", params));
        ASSERT_TRUE(response.contains("error")) << params.dump() << ": " << response.dump();
        EXPECT_EQ(response.at("error").at("code"), "invalid_request");
    }
    EXPECT_TRUE(call(runtime.get(), request("runtime.snapshot")).contains("result"));
}

TEST(MarketHistory, BoundsEventsAndBreaksOnGapsFailuresAndDisconnects)
{
    asterion::terminal::MarketHistory history;
    asterion::market::proto::EventBatch batch;
    batch.set_stream_id("fixture-stream");
    batch.set_latest_sequence(520);
    for (std::uint64_t i = 1; i <= 520; ++i) {
        auto* event = batch.add_events();
        event->set_sequence(i);
        auto* quote = event->mutable_quote()->mutable_quote();
        quote->mutable_instrument()->set_venue("SHFE");
        quote->mutable_instrument()->set_symbol("rb2610");
        quote->set_last("3510.00000001");
        quote->set_source_ms(1790582400000 + static_cast<std::int64_t>(i));
    }
    history.append(batch);
    auto view = history.snapshot();
    ASSERT_EQ(view.at("points").size(), 512);
    EXPECT_EQ(view.at("points").front().at("price"), "3510.00000001");
    EXPECT_EQ(view.at("points").front().at("timestamp_ns"), "1790582400009000000");
    EXPECT_EQ(history.cursor, 520);
    batch.clear_events();
    batch.set_latest_sequence(530);
    batch.set_gap(true);
    auto* event = batch.add_events();
    event->set_sequence(530);
    *event->mutable_quote()->mutable_quote() = asterion::market::proto::Quote{};
    history.append(batch);
    EXPECT_TRUE(history.snapshot().at("points").empty());
    EXPECT_TRUE(history.snapshot().at("interrupted"));
    batch.clear_events();
    batch.set_gap(false);
    batch.set_failed(true);
    history.append(batch);
    EXPECT_FALSE(history.snapshot().at("available"));
    history.interrupt();
    EXPECT_EQ(history.cursor, 0);
    EXPECT_TRUE(history.stream.empty());
    batch.set_stream_id("restarted");
    batch.set_failed(false);
    batch.set_latest_sequence(1);
    batch.add_events()->set_sequence(1);
    batch.mutable_events(0)->mutable_status()->set_phase("disconnected");
    history.append(batch);
    EXPECT_TRUE(history.snapshot().at("points").empty());
}

TEST(MarketHistory, RejectsBrokenOrderAndSkipsOutOfOrderQuotes)
{
    asterion::terminal::MarketHistory history;
    asterion::market::proto::EventBatch batch;
    batch.set_stream_id("fixture-stream");
    batch.set_latest_sequence(2);
    batch.add_events()->set_sequence(2);
    EXPECT_THROW(history.append(batch), std::invalid_argument);
    EXPECT_EQ(history.cursor, 0);
    batch.mutable_events(0)->set_sequence(1);
    auto* observation = batch.mutable_events(0)->mutable_quote();
    observation->set_out_of_order(true);
    observation->mutable_quote()->set_last("100");
    observation->mutable_quote()->set_source_ms(1000);
    history.append(batch);
    EXPECT_EQ(history.cursor, 1);
    EXPECT_TRUE(history.snapshot().at("points").empty());
    EXPECT_THROW(history.append(batch), std::invalid_argument);
}

TEST(MarketHistory, VolumeUsesPerContractDayBaselineAndResetsOnDiscontinuity)
{
    asterion::terminal::MarketHistory history;
    std::uint64_t sequence = 0;
    const auto append = [&](const char* symbol,
                            const char* day,
                            std::int64_t volume,
                            bool gap = false,
                            bool out_of_order = false) {
        asterion::market::proto::EventBatch batch;
        batch.set_stream_id("volume-fixture");
        batch.set_latest_sequence(++sequence);
        batch.set_gap(gap);
        auto* event = batch.add_events();
        event->set_sequence(sequence);
        auto* observation = event->mutable_quote();
        observation->set_out_of_order(out_of_order);
        auto* quote = observation->mutable_quote();
        quote->mutable_instrument()->set_venue("SHFE");
        quote->mutable_instrument()->set_symbol(symbol);
        quote->set_trading_day(day);
        quote->set_source_ms(1000 + static_cast<std::int64_t>(sequence));
        quote->set_last("100");
        quote->set_volume(volume);
        history.append(batch);
    };
    append("rb", "20260929", 1000);
    EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
    append("cu", "20260929", 9000);
    append("rb", "20260929", 1007);
    EXPECT_EQ(history.snapshot()["points"].back()["volume"], "7");
    append("rb", "20260929", 1007);
    EXPECT_EQ(history.snapshot()["points"].back()["volume"], "0");
    append("rb", "20260929", 3000, false, true);
    append("rb", "20260929", 1010);
    EXPECT_EQ(history.snapshot()["points"].back()["volume"], "3");
    append("rb", "20260930", 50);
    EXPECT_EQ(history.snapshot()["points"].size(), 2); // cu is retained.
    EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
    append("rb", "20260930", 40);
    EXPECT_EQ(history.snapshot()["points"].size(), 2);
    EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
    append("rb", "20260930", 45);
    EXPECT_EQ(history.snapshot()["points"].back()["volume"], "5");
    append("rb", "20260930", 100, true);
    EXPECT_EQ(history.snapshot()["points"].size(), 1);
    EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
    append("rb", "", 110);
    append("rb", "", 120);
    EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
    append("rb", "20260930", -1);
    append("rb", "20260930", 130);
    EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
}

namespace {

// A disposable root for this process, removed at exit. The services a
// Terminal starts exit with it.
class IsolatedNode
{
public:
    IsolatedNode()
        : path_(
              std::filesystem::temp_directory_path()
              / ("ast-node-test-" + asterion::unique_process_id()))
    {
        std::filesystem::create_directory(path_);
        ::setenv("ASTERION_ROOT", path_.c_str(), 1);
        ::setenv("ASTERION_TEST_NODE_ISOLATED", "1", 1);
    }

    ~IsolatedNode()
    {
        // Services still stopping may write once more.
        std::error_code ignored;
        for (int i = 0; i < 6; ++i) {
            std::filesystem::remove_all(path_, ignored);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!std::filesystem::exists(path_))
                break;
        }
    }

private:
    std::filesystem::path path_;
};

} // namespace

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    if (::testing::GTEST_FLAG(list_tests))
        return RUN_ALL_TESTS();
    // CTest runs each case with --isolated-node, so cases run in parallel, each
    // with its own root.
    std::optional<IsolatedNode> node;
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--isolated-node")
            node.emplace();
    // Direct invocation must never touch the user's own root.
    const char* directory = std::getenv("ASTERION_ROOT");
    const char* isolated = std::getenv("ASTERION_TEST_NODE_ISOLATED");
    if (!directory || !*directory || !isolated || std::string_view(isolated) != "1") {
        std::cerr
            << "Run terminal tests through CTest, with --isolated-node or " "tests/support/isolated_node.py; an isolated test root is required.\n";
        return 2;
    }
    return RUN_ALL_TESTS();
}
