#include "settings.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/foundation/serialization.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <fstream>
#include <cstdlib>
#include <asterion/protocol/trading.hpp>
#include <set>

namespace asterion::trading {
namespace fs = std::filesystem;

namespace {

Error invalid(const std::string& message)
{
    return Error(ErrorCode::invalid_request, message);
}

void exact(const Json& object, const std::set<std::string>& keys, const std::string& where)
{
    if (!object.is_object())
        throw invalid("invalid trading configuration: " + where);
    for (const auto& [key, _] : object.items())
        if (!keys.contains(key))
            throw invalid("trading configuration " + where + " has an unknown key " + key);
    for (const auto& key : keys)
        if (!object.contains(key))
            throw invalid("trading configuration " + where + " needs " + key);
}

fs::path required(const fs::path& path, bool directory)
{
    if (directory ? !fs::is_directory(path) : !fs::is_regular_file(path))
        throw invalid(
            std::string("the trading service needs the ") + (directory ? "directory " : "file ")
            + path.string());
    return path;
}

} // namespace

Settings load_settings(const fs::path& root, const std::string& account)
{
    validate_id(account);
    const auto file = root / "config" / "accounts" / (account + ".json");
    const auto where = file.string();
    if (!fs::is_regular_file(file) || fs::file_size(file) > 16384)
        throw invalid("the trading service needs the account configuration " + where);
    std::ifstream input(file, std::ios::binary);
    const std::string raw{std::istreambuf_iterator<char>(input), {}};
    Json document;
    try {
        document = parse_json(raw, 16384);
    } catch (const std::exception&) {
        throw invalid("cannot parse trading account configuration: " + where);
    }
    exact(document, {"bind", "port", "terminals", "broker", "environment"}, where);
    Settings result;
    result.account = account;
    result.identity = {
        {"account_id", account},
        {"broker", document.at("broker")},
        {"environment", document.at("environment")}
    };
    try {
        (void)protocol::encode_trading_account(result.identity);
    } catch (const std::exception& error) {
        throw invalid("invalid trading account configuration " + where + ": " + error.what());
    }
    const auto& bind = document.at("bind");
    const auto& port = document.at("port");
    if (!bind.is_string() || bind.get<std::string>().empty() || !port.is_number_unsigned()
        || port.get<std::uint64_t>() == 0 || port.get<std::uint64_t>() > 65535)
        throw invalid(
            "trading configuration " + where + " needs a bind address and a port in 1-65535");
    result.transport.bind = bind.get<std::string>();
    result.transport.port = static_cast<std::uint16_t>(port.get<std::uint64_t>());
    const auto& terminals = document.at("terminals");
    if (!terminals.is_array() || terminals.empty() || terminals.size() > 16)
        throw invalid(
            "trading configuration " + where + " needs 1-16 Terminal certificate fingerprints");
    for (const auto& terminal : terminals) {
        try {
            result.transport.tls.peers.push_back(
                ipc::normalize_fingerprint(terminal.get<std::string>()));
        } catch (const std::exception&) {
            throw invalid(
                "trading configuration " + where
                + " lists a Terminal fingerprint that is not 64 hex digits");
        }
    }
    const auto tls = root / "config" / "tls";
    result.transport.tls.certificate_file = required(tls / "server.crt", false).string();
    result.transport.tls.private_key_file = required(tls / "server.key", false).string();
    result.ctp_library = required(root / "lib" / "ctp-trader.so", false);
    const auto data = required(root / "data", true);
    // The account's record and the leases are the service's own: created on
    // first start, then only ever reused.
    result.journal = data / account;
    result.owners = data / "owners";
    for (const auto& directory : {result.journal, result.owners}) {
        if (fs::is_symlink(directory))
            throw invalid("the trading service refuses the symbolic link " + directory.string());
        create_directories_durably(directory);
    }
    const auto* credentials = std::getenv("CREDENTIALS_DIRECTORY");
    if (!credentials || !*credentials)
        throw invalid("the trading service needs systemd credentials for " + where);
    const auto secret = [&](const char* name) {
        const auto path = fs::path(credentials) / name;
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            throw invalid("the trading service needs the systemd credential " + path.string());
        std::string value{std::istreambuf_iterator<char>(stream), {}};
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
            value.pop_back();
        if (value.empty() || value.size() > 256 || value.find('\0') != std::string::npos)
            throw invalid("invalid systemd credential " + path.string());
        return value;
    };
    result.password = secret("password");
    result.auth_code = secret("auth_code");
    return result;
}

} // namespace asterion::trading
