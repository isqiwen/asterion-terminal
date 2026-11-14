#include "../apps/clients/terminal/native/python_runtime.hpp"
#include <asterion/system/environment.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/system/process/child.hpp>
#include <fstream>
#include <gtest/gtest.h>

using namespace asterion;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

// Opt in after prepare:python: this exercises the real locked downloads and
// installation, independently of the developer's prepared interpreter.
TEST(PythonRuntime, InstallsResearchAndReplacesTheEnvironmentOnUpgrade)
{
    const auto manifest = environment_variable("ASTERION_TEST_RUNTIME_MANIFEST");
    if (!manifest)
        GTEST_SKIP() << "set ASTERION_TEST_RUNTIME_MANIFEST to the prepared runtime directory";

    struct Temporary
    {
        ~Temporary()
        {
            fs::remove_all(root);
        }

        fs::path root = fs::temp_directory_path() / ("ast-runtime-" + unique_process_id());
    } temporary;

    const auto application = temporary.root / "application";
    const auto directory = temporary.root / "runtime";
    fs::create_directories(application);
    fs::copy_file(fs::path(*manifest) / "manifest.json", application / "manifest.json");
    fs::create_directory_symlink(fs::path(*manifest) / "app", application / "app");
    const auto install = [&] {
        terminal::PythonRuntime runtime(application, directory);
        EXPECT_EQ(runtime.status().at("state"), "missing");
        runtime.install();
        const auto deadline = std::chrono::steady_clock::now() + 10min;
        while (runtime.status().at("state") == "installing"
            && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(100ms);
        ASSERT_EQ(runtime.status().at("state"), "ready") << runtime.status().dump();
        ASSERT_TRUE(runtime.python());
        fs::remove(temporary.root / "imports.log");
        ChildProcess check(
            *runtime.python(),
            {"-I",
                "-B",
                "-c",
                "import asterion_engine, asterion_lab.__main__, quantstats, scipy, polars, duckdb\n" "from importlib.metadata import distributions\n" "names = {d.metadata['Name'].lower() for d in distributions()}\n" "assert not names & {'akshare', 'akracer', 'jsonpath', 'mini-racer', 'py-mini-racer'}\n"},
            false,
            temporary.root / "imports.log",
            true);
        ASSERT_TRUE(check.wait(60s));
        ASSERT_EQ(check.exit_code(), 0);
    };
    install();
    ASSERT_FALSE(HasFatalFailure());
    {
        terminal::PythonRuntime reopened(application, directory);
        ASSERT_EQ(reopened.status().at("state"), "ready");
        ASSERT_EQ(reopened.status().at("modules").size(), 3); // interpreter, core, reports
    }
    // A later manifest replaces the environment and discards obsolete downloads,
    // while reusing all archives still named by the lock.
    std::ofstream(application / "manifest.json", std::ios::app) << '\n';
    std::ofstream(directory / "python" / "obsolete-package") << "obsolete";
    fs::create_directory(directory / "downloads" / "obsolete-download");
    install();
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_FALSE(fs::exists(directory / "python" / "obsolete-package"));
    EXPECT_FALSE(fs::exists(directory / "downloads" / "obsolete-download"));
}
