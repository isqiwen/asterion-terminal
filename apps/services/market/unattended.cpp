#include "unattended.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/serialization.hpp>
#include <fstream>
#include <set>

namespace asterion::market_data {
namespace fs = std::filesystem;

namespace {

std::string secret(const fs::path& directory, const char* name)
{
    const auto file = directory / name;
    std::ifstream input(file, std::ios::binary);
    if (!input)
        throw Error(
            ErrorCode::invalid_request,
            "the unattended login needs the systemd credential " + file.string());
    std::string value{std::istreambuf_iterator<char>(input), {}};
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    if (value.empty() || value.size() > 256 || value.find('\0') != std::string::npos)
        throw Error(ErrorCode::invalid_request, "invalid systemd credential " + file.string());
    return value;
}

} // namespace

std::optional<Unattended>
load_unattended(const fs::path& configuration, const std::optional<fs::path>& credentials)
{
    if (configuration.empty() || !fs::exists(configuration))
        return std::nullopt;
    const auto where = configuration.string();
    if (!fs::is_regular_file(configuration) || fs::file_size(configuration) > 4096)
        throw Error(ErrorCode::invalid_request, "invalid market recording configuration: " + where);
    std::ifstream input(configuration, std::ios::binary);
    const std::string raw{std::istreambuf_iterator<char>(input), {}};
    const auto document = parse_json(raw, 4096);
    const std::set<std::string> keys{"market_front", "trade_front", "broker", "user", "app_id"};
    if (!document.is_object() || document.size() != keys.size())
        throw Error(ErrorCode::invalid_request, "invalid market recording configuration: " + where);
    for (const auto& key : keys)
        if (!document.contains(key) || !document.at(key).is_string()
            || (key != "app_id" && document.at(key).get<std::string>().empty()))
            throw Error(
                ErrorCode::invalid_request,
                "market recording configuration " + where + " needs " + key);
    if (!credentials)
        throw Error(
            ErrorCode::invalid_request,
            where + " asks for an unattended login, but the service has no systemd credentials");
    const auto broker = document.at("broker").get<std::string>();
    const auto user = document.at("user").get<std::string>();
    const auto app_id = document.at("app_id").get<std::string>();
    Unattended result;
    const auto password = secret(*credentials, "asterion.ctp.password");
    result.market = {document.at("market_front"), broker, user, password};
    result.catalog = {
        document.at("trade_front"),
        broker,
        user,
        password,
        app_id,
        app_id.empty() ? std::string{} : secret(*credentials, "asterion.ctp.auth_code")
    };
    return result;
}

} // namespace asterion::market_data
