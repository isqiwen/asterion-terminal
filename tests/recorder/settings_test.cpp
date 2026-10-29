#include "settings.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace asterion;
using recording::load_settings;

namespace {

struct Folder
{
    fs::path path = fs::temp_directory_path() / ("ast-login-" + unique_process_id());

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
        (void)load_settings(file, credentials);
    } catch (const Error& error) {
        return error.what();
    }
    return "accepted";
}

const char* configuration = R"({"directory":"/var/lib/asterion-recorder",
    "ctp_library":"/opt/asterion-recorder/current/ctp-md.so",
    "ctp_catalog_library":"/opt/asterion-recorder/current/ctp-trader.so",
    "bind":"0.0.0.0","port":7450,
    "counter":{"market_front":"tcp://md:1","trade_front":"tcp://td:2",
        "broker":"9999","user":"000001","app_id":"client_app"}})";

} // namespace

TEST(RecorderSettings, EverythingComesFromTheFileButTheSecrets)
{
    Folder folder;
    const auto file = folder.path / "recorder.json";
    write_file_durably(file, configuration);
    write_file_durably(folder.path / "credentials/asterion.ctp.password", "fixture-password\n");
    write_file_durably(folder.path / "credentials/asterion.ctp.auth_code", "fixture-auth");
    const auto login = load_settings(file, folder.path / "credentials");
    EXPECT_EQ(login.directory, "/var/lib/asterion-recorder");
    EXPECT_EQ(login.ctp_catalog_library, "/opt/asterion-recorder/current/ctp-trader.so");
    EXPECT_EQ(login.bind, "0.0.0.0");
    EXPECT_EQ(login.port, 7450);
    EXPECT_EQ(login.market.front, "tcp://md:1");
    EXPECT_EQ(login.market.password, "fixture-password");
    EXPECT_EQ(login.catalog.front, "tcp://td:2");
    EXPECT_EQ(login.catalog.auth_code, "fixture-auth");
    EXPECT_EQ(login.catalog.user, "000001");
}

TEST(RecorderSettings, ARefusalNamesTheFileOrTheCredentialItLacks)
{
    Folder folder;
    const auto file = folder.path / "recorder.json";
    EXPECT_NE(refusal(file, folder.path / "credentials").find(file.string()), std::string::npos);
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
    // The template before `recording-host.py login` fills in the counter.
    std::string blank = configuration;
    blank.replace(blank.find("tcp://md:1"), 10, "");
    write_file_durably(file, blank);
    EXPECT_NE(
        refusal(file, folder.path / "credentials").find("needs counter.market_front"),
        std::string::npos);
    std::string relative = configuration;
    relative.replace(relative.find("/var/lib/asterion-recorder"), 26, "data");
    write_file_durably(file, relative);
    EXPECT_NE(
        refusal(file, folder.path / "credentials").find("directory must be an absolute path"),
        std::string::npos);
}
