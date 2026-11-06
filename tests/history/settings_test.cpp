#include "settings.hpp"
#include <asterion/base/error.hpp>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using namespace asterion;
using asterion::history::load_settings;

namespace {

// An installation directory: config/, data/, lib/ and the credentials systemd
// would decrypt.
struct Installation
{
    fs::path root = fs::temp_directory_path() / ("ast-market-" + unique_process_id());
    fs::path file = root / "config/history.json";
    fs::path credentials = root / "credentials";
    // The settings file the installer writes for this installation.
    std::string configuration;
    static constexpr const char* template_text = R"({"bind":"0.0.0.0","port":7450,
    "clients":[{"fingerprint":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","role":"read"}],
    "acquisition":{"exchanges":[],"tushare":null}})";

    Installation()
    {
        for (const auto* folder : {"config", "data", "credentials", "config/tls"})
            fs::create_directories(root / folder);
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

TEST(HistorySettings, DataComesFromItsFileAndNoCounterSettingsExist)
{
    Installation installation;
    write_file_durably(installation.file, installation.configuration);
    const auto settings = load_settings(installation.root, std::nullopt);
    EXPECT_EQ(settings.directory, installation.root / "data");
    EXPECT_EQ(settings.bind, "0.0.0.0");
    EXPECT_EQ(settings.port, 7450);
    // Data may live on another disk; the path must be absolute and exist.
    auto moved = nlohmann::json::parse(installation.configuration);
    moved["data"] = (installation.root / "elsewhere").string();
    replace_file_durably(installation.file, moved.dump());
    EXPECT_TRUE(names(installation.refusal(std::nullopt), "elsewhere"));
    fs::create_directories(installation.root / "elsewhere");
    EXPECT_EQ(
        load_settings(installation.root, std::nullopt).directory,
        installation.root / "elsewhere");
    moved["data"] = "relative/data";
    replace_file_durably(installation.file, moved.dump());
    EXPECT_TRUE(names(installation.refusal(std::nullopt), "absolute data directory"));
    auto counter = nlohmann::json::parse(installation.configuration);
    counter["counter"] = nlohmann::json::object();
    replace_file_durably(installation.file, counter.dump());
    EXPECT_TRUE(names(installation.refusal(std::nullopt), "unknown key counter"));
}

TEST(HistorySettings, ARefusalNamesTheFileItRefuses)
{
    Installation installation;
    const auto& file = installation.file;
    EXPECT_TRUE(names(installation.refusal(std::nullopt), file.string()));
    write_file_durably(file, "{\"unexpected\":\"private-config-marker\"");
    const auto malformed = installation.refusal(std::nullopt);
    EXPECT_TRUE(names(malformed, file.string()));
    EXPECT_FALSE(names(malformed, "private-config-marker"));
}

TEST(HistorySettings, ClientRolesAndHostOnlyProviderCredentialsAreExplicit)
{
    Installation installation;
    auto config = nlohmann::json::parse(installation.configuration);
    const auto save = [&] {
        replace_file_durably(installation.file, config.dump());
    };
    config["clients"][0]["role"] = "manage";
    config["acquisition"] = {
        {"exchanges", {"SHFE", "CZCE"}},
        {"tushare", {{"requests_per_minute", 23U}}}
    };
    save();
    EXPECT_TRUE(
        names(installation.refusal(installation.credentials), "asterion.history.tushare.token"));
    replace_file_durably(
        installation.credentials / "asterion.history.tushare.token",
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
    fs::remove(installation.credentials / "asterion.history.tushare.token");
    EXPECT_TRUE(load_settings(installation.root, installation.credentials).tushare_token.empty());
    config["acquisition"]["exchanges"] = {"SHFE", "SHFE"};
    save();
    EXPECT_TRUE(
        names(installation.refusal(installation.credentials), "duplicate acquisition exchange"));
}
