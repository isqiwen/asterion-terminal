#include "settings.hpp"
#include <asterion/base/error.hpp>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace asterion;
using recording::load_settings;

namespace {

// An installation directory: config/, data/, lib/ and the credentials systemd
// would decrypt.
struct Installation
{
    fs::path root = fs::temp_directory_path() / ("ast-recorder-" + unique_process_id());
    fs::path file = root / "config/recorder.json";
    fs::path credentials = root / "credentials";

    Installation()
    {
        for (const auto* folder : {"config", "data", "lib", "credentials"})
            fs::create_directories(root / folder);
        write_file_durably(root / "lib/ctp-md.so", "md");
        write_file_durably(root / "lib/ctp-trader.so", "td");
    }

    ~Installation()
    {
        fs::remove_all(root);
    }

    std::string refusal(const std::optional<fs::path>& with) const
    {
        try {
            (void)load_settings(root, with);
        } catch (const Error& error) {
            return error.what();
        }
        return "accepted";
    }
};

const char* configuration = R"({"bind":"0.0.0.0","port":7450,
    "counter":{"market_front":"tcp://md:1","trade_front":"tcp://td:2",
        "broker":"9999","user":"000001","app_id":"client_app"}})";

bool names(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

TEST(RecorderSettings, PathsComeFromTheInstallationAndTheRestFromItsFile)
{
    Installation installation;
    write_file_durably(installation.file, configuration);
    write_file_durably(installation.credentials / "asterion.ctp.password", "fixture-password\n");
    write_file_durably(installation.credentials / "asterion.ctp.auth_code", "fixture-auth");
    const auto settings = load_settings(installation.root, installation.credentials);
    EXPECT_EQ(settings.directory, installation.root / "data");
    EXPECT_EQ(settings.ctp_library, installation.root / "lib/ctp-md.so");
    EXPECT_EQ(settings.ctp_catalog_library, installation.root / "lib/ctp-trader.so");
    EXPECT_EQ(settings.bind, "0.0.0.0");
    EXPECT_EQ(settings.port, 7450);
    EXPECT_EQ(settings.market.front, "tcp://md:1");
    EXPECT_EQ(settings.market.password, "fixture-password");
    EXPECT_EQ(settings.catalog.front, "tcp://td:2");
    EXPECT_EQ(settings.catalog.auth_code, "fixture-auth");
    EXPECT_EQ(settings.catalog.user, "000001");
}

TEST(RecorderSettings, ARefusalNamesTheFileTheKeyOrTheCredentialItLacks)
{
    Installation installation;
    const auto& file = installation.file;
    EXPECT_TRUE(names(installation.refusal(installation.credentials), file.string()));
    write_file_durably(file, configuration);
    EXPECT_TRUE(names(installation.refusal(std::nullopt), file.string()));
    const auto password = installation.credentials / "asterion.ctp.password";
    EXPECT_TRUE(names(installation.refusal(installation.credentials), password.string()));
    write_file_durably(password, "fixture-password");
    // An AppID needs its authentication code.
    EXPECT_TRUE(names(installation.refusal(installation.credentials), "asterion.ctp.auth_code"));
    write_file_durably(installation.credentials / "asterion.ctp.auth_code", "fixture-auth");
    // Paths are not settings.
    std::string extra = configuration;
    extra.insert(1, R"("directory":"/elsewhere",)");
    write_file_durably(file, extra);
    EXPECT_TRUE(names(installation.refusal(installation.credentials), "unknown key directory"));
    // The default file before `recording-host.py login` fills in the counter.
    std::string blank = configuration;
    blank.replace(blank.find("tcp://md:1"), 10, "");
    write_file_durably(file, blank);
    EXPECT_TRUE(
        names(installation.refusal(installation.credentials), "needs counter.market_front"));
    write_file_durably(file, configuration);
    fs::remove(installation.root / "lib/ctp-trader.so");
    EXPECT_TRUE(names(installation.refusal(installation.credentials), "lib/ctp-trader.so"));
}
