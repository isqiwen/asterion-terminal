#include "settings.hpp"
#include <asterion/base/error.hpp>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using namespace asterion;
using market_data::load_settings;

namespace {

// An installation directory: config/, data/, lib/ and the credentials systemd
// would decrypt.
struct Installation
{
    fs::path root = fs::temp_directory_path() / ("ast-market-" + unique_process_id());
    fs::path file = root / "config/market.json";
    fs::path credentials = root / "credentials";
    // The settings file the installer writes for this installation.
    std::string configuration;
    static constexpr const char* template_text = R"({"bind":"0.0.0.0","port":7450,
    "clients":[{"fingerprint":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","role":"read"}],
    "acquisition":{"exchanges":[],"tushare":null},
    "counter":{"market_front":"tcp://md:1","trade_front":"tcp://td:2",
        "broker":"9999","user":"000001","app_id":"client_app"}})";

    Installation()
    {
        for (const auto* folder : {"config", "data", "lib", "credentials", "config/tls"})
            fs::create_directories(root / folder);
        write_file_durably(root / "lib/ctp-md.so", "md");
        write_file_durably(root / "lib/ctp-trader.so", "td");
        write_file_durably(root / "config/tls/server.crt", "test");
        write_file_durably(root / "config/tls/server.key", "test");
        auto value = nlohmann::json::parse(template_text);
        value["data"] = (root / "data").string();
        configuration = value.dump();
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

bool names(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

TEST(MarketSettings, ProgramsComeFromTheInstallationAndDataAndTheRestFromItsFile)
{
    Installation installation;
    write_file_durably(installation.file, installation.configuration);
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
    // Data may live on another disk; the path must be absolute and exist.
    auto moved = nlohmann::json::parse(installation.configuration);
    moved["data"] = (installation.root / "elsewhere").string();
    replace_file_durably(installation.file, moved.dump());
    EXPECT_TRUE(names(installation.refusal(installation.credentials), "elsewhere"));
    fs::create_directories(installation.root / "elsewhere");
    const auto elsewhere = load_settings(installation.root, installation.credentials);
    EXPECT_EQ(elsewhere.directory, installation.root / "elsewhere");
    moved["data"] = "relative/data";
    replace_file_durably(installation.file, moved.dump());
    EXPECT_TRUE(names(installation.refusal(installation.credentials), "absolute data directory"));
}

TEST(MarketSettings, ARefusalNamesTheFileTheKeyOrTheCredentialItLacks)
{
    Installation installation;
    const auto& file = installation.file;
    EXPECT_TRUE(names(installation.refusal(installation.credentials), file.string()));
    write_file_durably(file, "{\"unexpected\":\"private-config-marker\"");
    const auto malformed = installation.refusal(installation.credentials);
    EXPECT_TRUE(names(malformed, file.string()));
    EXPECT_FALSE(names(malformed, "private-config-marker"));
    replace_file_durably(file, installation.configuration);
    EXPECT_TRUE(names(installation.refusal(std::nullopt), file.string()));
    const auto password = installation.credentials / "asterion.ctp.password";
    EXPECT_TRUE(names(installation.refusal(installation.credentials), password.string()));
    write_file_durably(password, "fixture-password");
    // An AppID needs its authentication code.
    EXPECT_TRUE(names(installation.refusal(installation.credentials), "asterion.ctp.auth_code"));
    write_file_durably(installation.credentials / "asterion.ctp.auth_code", "fixture-auth");
    // Paths are not settings.
    std::string extra = installation.configuration;
    extra.insert(1, R"("directory":"/elsewhere",)");
    write_file_durably(file, extra);
    EXPECT_TRUE(names(installation.refusal(installation.credentials), "unknown key directory"));
    // The default file before `recording-host.py login` fills in the counter.
    std::string blank = installation.configuration;
    blank.replace(blank.find("tcp://md:1"), 10, "");
    write_file_durably(file, blank);
    EXPECT_TRUE(
        names(installation.refusal(installation.credentials), "needs counter.market_front"));
    write_file_durably(file, installation.configuration);
    fs::remove(installation.root / "lib/ctp-trader.so");
    EXPECT_TRUE(names(installation.refusal(installation.credentials), "lib/ctp-trader.so"));
}

TEST(MarketSettings, ClientRolesAndHostOnlyProviderCredentialsAreExplicit)
{
    Installation installation;
    auto config = nlohmann::json::parse(installation.configuration);
    const auto save = [&] {
        replace_file_durably(installation.file, config.dump());
    };
    replace_file_durably(installation.credentials / "asterion.ctp.password", "fixture-password");
    replace_file_durably(installation.credentials / "asterion.ctp.auth_code", "fixture-auth");
    config["clients"][0]["role"] = "manage";
    config["acquisition"] = {
        {"exchanges", {"SHFE", "CZCE"}},
        {"tushare", {{"requests_per_minute", 23U}}}
    };
    save();
    EXPECT_TRUE(
        names(installation.refusal(installation.credentials), "asterion.market.tushare.token"));
    replace_file_durably(
        installation.credentials / "asterion.market.tushare.token",
        "fixture-token");
    auto settings = load_settings(installation.root, installation.credentials);
    EXPECT_TRUE(settings.clients.at(std::string(64, 'a')));
    EXPECT_EQ(settings.tls.peers, (std::vector<std::string>{std::string(64, 'a')}));
    EXPECT_EQ(settings.tushare_token, "fixture-token");
    EXPECT_EQ(settings.tushare_requests_per_minute, 23U);
    config["acquisition"]["tushare"]["token"] = "must-not-be-in-config";
    save();
    EXPECT_TRUE(names(installation.refusal(installation.credentials), "unknown key token"));
    config["acquisition"]["tushare"].erase("token");
    config["clients"][0]["role"] = "admin";
    save();
    EXPECT_TRUE(names(installation.refusal(installation.credentials), "invalid role"));
    config["clients"][0]["role"] = "read";
    config["clients"].push_back(config["clients"][0]);
    save();
    EXPECT_TRUE(names(installation.refusal(installation.credentials), "duplicate clients"));
    config["clients"].erase(1);
    config["acquisition"]["tushare"] = nullptr;
    save();
    fs::remove(installation.credentials / "asterion.market.tushare.token");
    EXPECT_TRUE(load_settings(installation.root, installation.credentials).tushare_token.empty());
    config["acquisition"]["exchanges"] = {"SHFE", "SHFE"};
    save();
    EXPECT_TRUE(
        names(installation.refusal(installation.credentials), "duplicate acquisition exchange"));
}
