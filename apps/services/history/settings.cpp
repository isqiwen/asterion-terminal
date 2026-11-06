#include "settings.hpp"
#include <asterion/base/error.hpp>
#include <asterion/base/serialization.hpp>
#include <fstream>
#include <set>
#include <algorithm>

namespace asterion::history {
namespace fs = std::filesystem;

namespace {

std::string secret(const fs::path& directory, const char* name)
{
    const auto file = directory / name;
    std::ifstream input(file, std::ios::binary);
    if (!input)
        throw Error(
            ErrorCode::invalid_request,
            "the market needs the systemd credential " + file.string());
    std::string value{std::istreambuf_iterator<char>(input), {}};
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    if (value.empty() || value.size() > 256 || value.find('\0') != std::string::npos)
        throw Error(ErrorCode::invalid_request, "invalid systemd credential " + file.string());
    return value;
}

} // namespace

namespace {

std::string text(const nlohmann::json& object, const std::string& key, const std::string& where)
{
    if (!object.contains(key) || !object.at(key).is_string())
        throw Error(ErrorCode::invalid_request, "market configuration " + where + " needs " + key);
    return object.at(key).get<std::string>();
}

void exact(
    const nlohmann::json& object,
    const std::set<std::string>& keys,
    const std::string& where)
{
    if (!object.is_object())
        throw Error(ErrorCode::invalid_request, "invalid market configuration: " + where);
    for (const auto& [key, _] : object.items())
        if (!keys.contains(key))
            throw Error(
                ErrorCode::invalid_request,
                "market configuration " + where + " has an unknown key " + key);
    for (const auto& key : keys)
        if (!object.contains(key))
            throw Error(
                ErrorCode::invalid_request,
                "market configuration " + where + " needs " + key);
}

} // namespace

Settings load_settings(const fs::path& root, const std::optional<fs::path>& credentials)
{
    const auto file = root / "config" / "history.json";
    const auto where = file.string();
    if (!fs::is_regular_file(file) || fs::file_size(file) > 16384)
        throw Error(ErrorCode::invalid_request, "invalid market configuration: " + where);
    std::ifstream input(file, std::ios::binary);
    const std::string raw{std::istreambuf_iterator<char>(input), {}};
    Json document;
    try {
        document = parse_json(raw, 16384);
    } catch (const std::exception&) {
        throw Error(ErrorCode::invalid_request, "invalid market configuration: " + where);
    }
    exact(document, {"bind", "port", "data", "counter", "clients", "acquisition"}, where);
    Settings result;
    result.directory = text(document, "data", where);
    if (!result.directory.is_absolute())
        throw Error(
            ErrorCode::invalid_request,
            "market configuration " + where + " needs an absolute data directory");
    result.ctp_library = root / "lib" / "ctp-md.so";
    result.ctp_catalog_library = root / "lib" / "ctp-trader.so";
    if (!fs::is_directory(result.directory))
        throw Error(
            ErrorCode::invalid_request,
            "the market needs the directory " + result.directory.string());
    for (const auto& library : {result.ctp_library, result.ctp_catalog_library})
        if (!fs::is_regular_file(library))
            throw Error(
                ErrorCode::invalid_request,
                "the market needs the library " + library.string());
    result.bind = text(document, "bind", where);
    const auto& port = document.at("port");
    if (result.bind.empty() || !port.is_number_unsigned() || port.get<std::uint64_t>() == 0
        || port.get<std::uint64_t>() > 65535)
        throw Error(
            ErrorCode::invalid_request,
            "market configuration " + where + " needs a bind address and a port in 1-65535");
    result.port = static_cast<std::uint16_t>(port.get<std::uint64_t>());
    const auto& counter = document.at("counter");
    exact(counter, {"market_front", "trade_front", "broker", "user", "app_id"}, where);
    for (const auto* key : {"market_front", "trade_front", "broker", "user"})
        if (text(counter, key, where).empty())
            throw Error(
                ErrorCode::invalid_request,
                "market configuration " + where + " needs counter." + key);
    if (!credentials)
        throw Error(
            ErrorCode::invalid_request,
            "the market has no systemd credentials for " + where);
    const auto broker = text(counter, "broker", where);
    const auto user = text(counter, "user", where);
    const auto app_id = text(counter, "app_id", where);
    const auto password = secret(*credentials, "asterion.ctp.password");
    result.market = {text(counter, "market_front", where), broker, user, password};
    result.catalog = {
        text(counter, "trade_front", where),
        broker,
        user,
        password,
        app_id,
        app_id.empty() ? std::string{} : secret(*credentials, "asterion.ctp.auth_code")
    };
    const auto& clients = document.at("clients");
    if (!clients.is_array() || clients.empty() || clients.size() > 64)
        throw Error(ErrorCode::invalid_request, where + " needs 1-64 clients");
    for (const auto& client : clients) {
        exact(client, {"fingerprint", "role"}, where);
        const auto role = text(client, "role", where);
        std::string fingerprint;
        try {
            fingerprint = ipc::normalize_fingerprint(text(client, "fingerprint", where));
        } catch (...) {
            throw Error(ErrorCode::invalid_request, where + " has an invalid client fingerprint");
        }
        if ((role != "read" && role != "manage")
            || !result.clients.emplace(fingerprint, role == "manage").second)
            throw Error(
                ErrorCode::invalid_request,
                where + " has duplicate clients or an invalid role");
        result.tls.peers.push_back(fingerprint);
    }
    const auto certificate = root / "config/tls/server.crt", key = root / "config/tls/server.key";
    for (const auto& path : {certificate, key})
        if (!fs::is_regular_file(path))
            throw Error(ErrorCode::invalid_request, "History needs TLS identity " + path.string());
    result.tls.certificate_file = certificate.string();
    result.tls.private_key_file = key.string();
    const auto& acquisition = document.at("acquisition");
    exact(acquisition, {"exchanges", "tushare"}, where);
    const auto& exchanges = acquisition.at("exchanges");
    if (!exchanges.is_array() || exchanges.size() > 6)
        throw Error(ErrorCode::invalid_request, where + " needs up to six acquisition exchanges");
    std::set<std::string> seen;
    for (const auto& exchange : exchanges) {
        const auto value = exchange.is_string() ? exchange.get<std::string>() : std::string{};
        if (!std::set<std::string>{"SHFE", "INE", "CZCE", "DCE", "CFFEX", "GFEX"}.contains(value)
            || !seen.insert(value).second)
            throw Error(
                ErrorCode::invalid_request,
                where + " has an unknown or duplicate acquisition exchange");
        result.exchanges.push_back(value);
    }
    const auto& tushare = acquisition.at("tushare");
    if (!tushare.is_null()) {
        exact(tushare, {"requests_per_minute"}, where);
        const auto& budget = tushare.at("requests_per_minute");
        if (!budget.is_number_unsigned() || budget.get<std::uint64_t>() < 1
            || budget.get<std::uint64_t>() > 500)
            throw Error(
                ErrorCode::invalid_request,
                where + " needs Tushare requests_per_minute in 1-500");
        result.tushare_requests_per_minute = budget.get<unsigned>();
        result.tushare_token = secret(*credentials, "asterion.history.tushare.token");
        if (std::any_of(
                result.tushare_token.begin(),
                result.tushare_token.end(),
                [](unsigned char c) {
                    return c <= 32 || c == 127;
                }))
            throw Error(
                ErrorCode::invalid_request,
                "invalid systemd credential "
                    + (*credentials / "asterion.history.tushare.token").string());
    }
    return result;
}

} // namespace asterion::history
