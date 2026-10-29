#include "settings.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/serialization.hpp>
#include <fstream>
#include <set>

namespace asterion::recording {
namespace fs = std::filesystem;

namespace {

std::string secret(const fs::path& directory, const char* name)
{
    const auto file = directory / name;
    std::ifstream input(file, std::ios::binary);
    if (!input)
        throw Error(
            ErrorCode::invalid_request,
            "the recorder needs the systemd credential " + file.string());
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
        throw Error(
            ErrorCode::invalid_request,
            "recorder configuration " + where + " needs " + key);
    return object.at(key).get<std::string>();
}

void exact(
    const nlohmann::json& object,
    const std::set<std::string>& keys,
    const std::string& where)
{
    if (!object.is_object() || object.size() != keys.size())
        throw Error(ErrorCode::invalid_request, "invalid recorder configuration: " + where);
    for (const auto& [key, _] : object.items())
        if (!keys.contains(key))
            throw Error(
                ErrorCode::invalid_request,
                "recorder configuration " + where + " has an unknown key " + key);
}

fs::path absolute(const nlohmann::json& object, const std::string& key, const std::string& where)
{
    const fs::path path = text(object, key, where);
    if (!path.is_absolute())
        throw Error(
            ErrorCode::invalid_request,
            "recorder configuration " + where + ": " + key + " must be an absolute path");
    return path;
}

} // namespace

Settings load_settings(const fs::path& file, const std::optional<fs::path>& credentials)
{
    const auto where = file.string();
    if (!fs::is_regular_file(file) || fs::file_size(file) > 16384)
        throw Error(ErrorCode::invalid_request, "invalid recorder configuration: " + where);
    std::ifstream input(file, std::ios::binary);
    const std::string raw{std::istreambuf_iterator<char>(input), {}};
    const auto document = parse_json(raw, 16384);
    exact(
        document,
        {"directory", "ctp_library", "ctp_catalog_library", "bind", "port", "counter"},
        where);
    Settings result;
    result.directory = absolute(document, "directory", where);
    result.ctp_library = absolute(document, "ctp_library", where);
    result.ctp_catalog_library = absolute(document, "ctp_catalog_library", where);
    result.bind = text(document, "bind", where);
    const auto& port = document.at("port");
    if (result.bind.empty() || !port.is_number_unsigned() || port.get<std::uint64_t>() == 0
        || port.get<std::uint64_t>() > 65535)
        throw Error(
            ErrorCode::invalid_request,
            "recorder configuration " + where + " needs a bind address and a port in 1-65535");
    result.port = static_cast<std::uint16_t>(port.get<std::uint64_t>());
    const auto& counter = document.at("counter");
    exact(counter, {"market_front", "trade_front", "broker", "user", "app_id"}, where);
    for (const auto* key : {"market_front", "trade_front", "broker", "user"})
        if (text(counter, key, where).empty())
            throw Error(
                ErrorCode::invalid_request,
                "recorder configuration " + where + " needs counter." + key);
    if (!credentials)
        throw Error(
            ErrorCode::invalid_request,
            "the recorder has no systemd credentials for " + where);
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
    return result;
}

} // namespace asterion::recording
