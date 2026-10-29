#include "unattended.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace asterion;
using market_data::load_unattended;

namespace {

struct Folder
{
    fs::path path = fs::temp_directory_path() / ("ast-unattended-" + unique_process_id());

    Folder()
    {
        fs::create_directories(path / "credentials");
    }

    ~Folder()
    {
        fs::remove_all(path);
    }
};

std::string refusal(const fs::path& file, const std::optional<fs::path>& credentials)
{
    try {
        (void)load_unattended(file, credentials);
    } catch (const Error& error) {
        return error.what();
    }
    return "accepted";
}

const char* configuration = R"({"market_front":"tcp://md:1","trade_front":"tcp://td:2",
    "broker":"9999","user":"000001","app_id":"client_app"})";

} // namespace

TEST(Unattended, ANodeWithoutTheFileDoesNotLogInByItself)
{
    Folder folder;
    EXPECT_FALSE(load_unattended(folder.path / "market-recording.json", std::nullopt));
}

TEST(Unattended, TheLoginComesFromTheFileAndTheSystemdCredentials)
{
    Folder folder;
    const auto file = folder.path / "market-recording.json";
    write_file_durably(file, configuration);
    write_file_durably(folder.path / "credentials/asterion.ctp.password", "fixture-password\n");
    write_file_durably(folder.path / "credentials/asterion.ctp.auth_code", "fixture-auth");
    const auto login = load_unattended(file, folder.path / "credentials");
    ASSERT_TRUE(login);
    EXPECT_EQ(login->market.front, "tcp://md:1");
    EXPECT_EQ(login->market.password, "fixture-password");
    EXPECT_EQ(login->catalog.front, "tcp://td:2");
    EXPECT_EQ(login->catalog.auth_code, "fixture-auth");
    EXPECT_EQ(login->catalog.user, "000001");
}

TEST(Unattended, ARefusalNamesTheFileOrTheCredentialItLacks)
{
    Folder folder;
    const auto file = folder.path / "market-recording.json";
    write_file_durably(file, configuration);
    EXPECT_NE(refusal(file, std::nullopt).find(file.string()), std::string::npos);
    const auto password = folder.path / "credentials/asterion.ctp.password";
    EXPECT_NE(
        refusal(file, folder.path / "credentials").find(password.string()),
        std::string::npos);
    write_file_durably(password, "fixture-password");
    // An AppID needs its authentication code.
    EXPECT_NE(
        refusal(file, folder.path / "credentials").find("asterion.ctp.auth_code"),
        std::string::npos);
    write_file_durably(file, R"({"market_front":"tcp://md:1","broker":"9999"})");
    EXPECT_NE(refusal(file, folder.path / "credentials").find(file.string()), std::string::npos);
}
