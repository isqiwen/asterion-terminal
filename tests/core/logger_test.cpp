#include <asterion/base/time.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/process/owner.hpp>
#include <asterion/system/trace.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <ctime>
#include <fstream>
#include <regex>
#include <thread>
using namespace asterion;
namespace fs = std::filesystem;

namespace {

// An installation directory with config/logging.json; the log is reopened
// for it in each test and back to standard error afterwards.
struct Installation : testing::Test
{
    fs::path root = fs::temp_directory_path()
        / ("asterion-log-test-" + std::to_string(SystemClock{}.monotonic_now()));

    void SetUp() override
    {
        fs::create_directories(root / "config");
    }

    void TearDown() override
    {
        log::init(root / "none", "test");
        std::error_code error;
        fs::remove_all(root, error);
    }

    void configure(const std::string& text) const
    {
        std::ofstream(root / "config" / "logging.json", std::ios::trunc) << text;
    }

    std::string read(const fs::path& file) const
    {
        std::ifstream input(file);
        return {std::istreambuf_iterator<char>(input), {}};
    }

    std::string refusal(const std::string& text) const
    {
        configure(text);
        try {
            log::init(root, "test");
        } catch (const Error& error) {
            return error.what();
        }
        return "accepted";
    }
};

std::string today()
{
    const auto now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char date[16];
    std::strftime(date, sizeof date, "%Y-%m-%d", &local);
    return date;
}

} // namespace

TEST_F(Installation, ConsoleAndFileHaveTheirOwnSwitchLevelAndPattern)
{
    configure(R"({"console":{"enabled":true,"level":"error","pattern":"console %l %v"},
        "file":{"enabled":true,"level":"debug","pattern":"[%l] %n %v",
                "path":"../logs/{program}-{pid}.log","retention_days":7}})");
    log::init(root, "recorder");
    testing::internal::CaptureStderr();
    log::debug("catalog read: contracts={}", 1234);
    log::error("write failed: {}", "disk full");
    EXPECT_EQ(testing::internal::GetCapturedStderr(), "console error write failed: disk full\n");
    // The relative path is from the program's bin/; written at once.
    const auto file = root / "logs"
        / ("recorder-" + std::to_string(current_process_id()) + "_" + today() + ".log");
    EXPECT_EQ(log::file(root, "recorder", current_process_id()), file);
    EXPECT_EQ(
        read(file),
        "[debug] recorder catalog read: contracts=1234\n" "[error] recorder write failed: disk full\n");
}

TEST_F(Installation, TheDefaultPatternIsLocalTimeLevelAndProgram)
{
    configure(R"({"console":{"enabled":false,"level":"info"},
        "file":{"enabled":true,"level":"info","path":"../logs/{program}.log","retention_days":30}})");
    log::init(root, "market");
    log::info("market started");
    log::debug("not written at info");
    const auto line = read(root / "logs" / ("market_" + today() + ".log"));
    EXPECT_TRUE(
        std::regex_match(
            line,
            std::regex(
                R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} [+-]\d{2}:\d{2} info market market started\n)")))
        << line;
}

TEST_F(Installation, OldDailyFilesGoWhenTheLogOpensAndOtherFilesStay)
{
    fs::create_directories(root / "logs");
    const auto dated = [&](int days_ago) {
        const auto then = std::time(nullptr) - days_ago * 24 * 3600;
        std::tm local{};
        localtime_r(&then, &local);
        char name[48];
        std::strftime(name, sizeof name, "other-1_%Y-%m-%d.log", &local);
        return root / "logs" / name;
    };
    std::ofstream(dated(2)) << "recent\n";
    std::ofstream(dated(9)) << "expired\n";
    std::ofstream(root / "logs" / "notes.log") << "kept\n";
    configure(R"({"console":{"enabled":false,"level":"info"},
        "file":{"enabled":true,"level":"info","path":"../logs/{program}-{pid}.log","retention_days":7}})");
    log::init(root, "trading");
    EXPECT_TRUE(fs::exists(dated(2)));
    EXPECT_FALSE(fs::exists(dated(9)));
    EXPECT_TRUE(fs::exists(root / "logs" / "notes.log"));
}

TEST_F(Installation, AnAbsolutePathIsUsedAsItIs)
{
    const auto elsewhere = root / "elsewhere";
    configure(
        R"({"console":{"enabled":false,"level":"info"},"file":{"enabled":true,"level":"info","path":")"
        + (elsewhere / "agent.log").string() + R"(","retention_days":30}})");
    log::init(root, "agent");
    log::warn("agent recovery required");
    EXPECT_NE(
        read(elsewhere / ("agent_" + today() + ".log")).find("agent recovery required"),
        std::string::npos);
}

TEST_F(Installation, ARefusalNamesTheFileAndTheSetting)
{
    const auto file = (root / "config" / "logging.json").string();
    const std::string console = R"("console":{"enabled":false,"level":"info"})";
    const std::string sink = R"("path":"../logs/x.log","retention_days":30)";
    const auto refused = [&](const std::string& text, const std::string& what) {
        const auto message = refusal(text);
        EXPECT_NE(message.find(file), std::string::npos) << message;
        EXPECT_NE(message.find(what), std::string::npos) << message;
    };
    refused(
        "{" + console + R"(,"file":{"enabled":true,"level":"loud",)" + sink + "}}",
        "file.level");
    refused(
        "{" + console + R"(,"file":{"enabled":"yes","level":"info",)" + sink + "}}",
        "file.enabled");
    refused(
        "{" + console
            + R"(,"file":{"enabled":true,"level":"info","path":"../x.log","retention_days":0}})",
        "file.retention_days");
    refused(
        "{" + console + R"(,"format":"text","file":{"enabled":true,"level":"info",)" + sink + "}}",
        "format is not a setting");
    refused("{" + console + "}", "file is missing");
    refused(
        R"({"console":{"enabled":false,"level":"info","path":"x"},"file":{"enabled":true,"level":"info",)"
            + sink + "}}",
        "console.path is not a setting");
}

TEST_F(Installation, WithoutAConfigurationRecordsGoToStandardError)
{
    log::init(root, "test");
    testing::internal::CaptureStderr();
    log::info("no configuration: {}", 1);
    const auto output = testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find(" info test no configuration: 1\n"), std::string::npos) << output;
    EXPECT_EQ(log::file(root, "test", 1), fs::path{});
}

TEST(Logger, TraceScopesRestoreAcrossFailuresAndThreads)
{
    const std::string outer = "test.outer";
    TraceScope scope(outer);
    auto fail = [] {
        const std::string nested = "test.nested";
        TraceScope child(nested);
        EXPECT_EQ(current_trace_id(), nested);
        const auto first = next_correlation_id(), second = next_correlation_id();
        EXPECT_NE(first, second);
        EXPECT_NO_THROW(validate_id(first));
        throw Error(ErrorCode::conflict, "test request failure");
    };
    EXPECT_THROW(fail(), Error);
    EXPECT_EQ(current_trace_id(), outer);
    std::jthread worker([&] {
        EXPECT_TRUE(current_trace_id().empty());
        EXPECT_THROW(fail(), Error);
        EXPECT_TRUE(current_trace_id().empty());
    });
    worker.join();
    EXPECT_EQ(current_trace_id(), outer);
}
