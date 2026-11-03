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
