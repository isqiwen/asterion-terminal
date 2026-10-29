#include "ctp_connections.hpp"
#include "node_paths.hpp"
#include <algorithm>
#include <asterion/foundation/id.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <cctype>
#include <asterion/kernel/process/file_lock.hpp>
#include <charconv>
#include <fstream>
#include <regex>

namespace asterion::terminal {
namespace fs = std::filesystem;

std::filesystem::path ctp_account_directory(const std::string& account, bool create)
{
    // The account id is the directory name: one trading record per CTP account.
    if (account.empty() || account.size() > 64
        || account.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
            != std::string::npos)
        throw std::invalid_argument("invalid CTP connection identity");
    const auto root = program_directory("terminal") / "data" / "accounts";
    const auto group = root / "ctp";
    const auto directory = group / account;
    if (!create)
        return directory;
    for (const auto& path : {root, group, directory}) {
        if (fs::is_symlink(path))
            throw std::invalid_argument("invalid managed account directory");
        create_directories_durably(path);
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
    }
    return directory;
}

namespace {

void safe(const fs::path& path)
{
    if (!path.is_absolute() || fs::is_symlink(path))
        throw std::invalid_argument("invalid CTP connection storage");
}

// CTP identities: short printable text without spaces.
bool identity(const std::string& text, std::size_t limit, bool required)
{
    return (required ? !text.empty() : true) && text.size() <= limit
        && std::ranges::none_of(text, [](unsigned char c) {
               return c <= ' ' || c > '~';
           });
}

bool front(const std::string& text)
{
    const auto colon = text.rfind(':');
    if (!text.starts_with("tcp://") || text.size() > 64 || colon == std::string::npos || colon <= 6)
        return false;
    const auto host = text.substr(6, colon - 6), port = text.substr(colon + 1);
    int number = 0;
    const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
    return error == std::errc{} && end == port.data() + port.size() && number >= 1
        && number <= 65535 && std::ranges::all_of(host, [](unsigned char c) {
               return std::isalnum(c) || c == '.' || c == '-';
           });
}

// Each field fails with its own message: the form shows which one to fix.
void validate(const CtpConnection& c)
{
    if (c.name.empty() || c.name.size() > 128 || c.name.find('\0') != std::string::npos)
        throw std::invalid_argument("CTP account name must be 1 to 128 characters");
    if (!identity(c.broker_id, 10, true))
        throw std::invalid_argument("CTP broker id must be 1 to 10 characters without spaces");
    if (!identity(c.user_id, 15, true))
        throw std::invalid_argument("CTP investor id must be 1 to 15 characters without spaces");
    if (!identity(c.app_id, 32, true))
        throw std::invalid_argument("CTP AppID must be 1 to 32 characters without spaces");
    if (!front(c.trade_front))
        throw std::invalid_argument("CTP trade front must look like tcp://host:port");
    if (!front(c.market_front))
        throw std::invalid_argument("CTP market front must look like tcp://host:port");
}

void validate(const MarketCredentials& credentials)
{
    if (credentials.password.empty() || credentials.password.size() > 40
        || credentials.password.find('\0') != std::string::npos || credentials.auth_code.empty()
        || credentials.auth_code.size() > 16
        || credentials.auth_code.find('\0') != std::string::npos)
        throw Error(ErrorCode::invalid_request, "enter the market password and authorization code");
}

Json encode(const CtpConnection& c)
{
    Json result{
        {"id", c.id},
        {"name", c.name},
        {"broker_id", c.broker_id},
        {"user_id", c.user_id},
        {"app_id", c.app_id},
        {"trade_front", c.trade_front},
        {"market_front", c.market_front},
        {"revision", c.revision}
    };
    if (c.trading)
        result["trading"] = {{"node", c.trading->node}, {"service", c.trading->service}};
    return result;
}

} // namespace

std::string CtpConnections::credential_account(const CtpConnection& connection) const
{
    // Bind to both account and destinations. Renaming preserves the login, while
    // edits to counter identity or either front cannot reuse its secrets.
    const auto identity = Json::array(
        {connection.id,
            connection.broker_id,
            connection.user_id,
            connection.app_id,
            connection.trade_front,
            connection.market_front});
    return sha256_bytes(directory_.string()).substr(0, 16) + "/"
        + sha256_bytes(identity.dump()).substr(0, 32);
}

MarketCredentials CtpConnections::market_credentials(const CtpConnection& connection) const
{
    if (!remembered_)
        throw Error(ErrorCode::unavailable, "credential store unavailable");
    const auto saved = remembered_->load(credential_account(connection));
    if (!saved)
        throw Error(
            ErrorCode::not_found,
            "saved market credentials are unavailable; enter them again");
    try {
        const auto value = parse_json(*saved, 4096);
        if (value.size() != 2)
            throw Error(ErrorCode::invalid_request, "invalid saved market credentials");
        MarketCredentials credentials{value.at("password"), value.at("auth_code")};
        validate(credentials);
        return credentials;
    } catch (const Json::exception&) {
        // Parser diagnostics can quote input. Never expose keychain contents.
        throw Error(ErrorCode::invalid_request, "invalid saved market credentials");
    }
}

void CtpConnections::remember_market_credentials(
    const std::string& id,
    const MarketCredentials& credentials)
{
    validate(credentials);
    if (!remembered_)
        throw Error(ErrorCode::unavailable, "credential store unavailable");
    FileLock lock(directory_, "connections.lock");
    remembered_->store(
        credential_account(get(id)),
        Json{{"password", credentials.password}, {"auth_code", credentials.auth_code}}.dump());
}

void CtpConnections::forget_market_credentials(const std::string& id)
{
    if (!remembered_)
        throw Error(ErrorCode::unavailable, "credential store unavailable");
    FileLock lock(directory_, "connections.lock");
    remembered_->erase(credential_account(get(id)));
}

void CtpConnections::forget_remembered()
{
    safe(directory_);
    if (!remembered_ || !fs::is_directory(directory_))
        return;
    FileLock lock(directory_, "connections.lock");
    // An unreadable connection names no account; it fails here rather than
    // leaving its secrets behind once the file is deleted.
    for (const auto& entry : fs::directory_iterator(directory_))
        if (entry.path().extension() == ".json")
            remembered_->erase(credential_account(get(entry.path().stem().string())));
}

fs::path CtpConnections::path(const std::string& id) const
{
    if (!std::regex_match(id, std::regex("[A-Za-z0-9][A-Za-z0-9_-]{0,63}")))
        throw std::invalid_argument("invalid CTP connection identity");
    safe(directory_);
    const auto file = directory_ / (id + ".json");
    safe(file);
    return file;
}

CtpConnection CtpConnections::get(const std::string& id) const
{
    const auto file = path(id);
    if (!fs::is_regular_file(file) || fs::file_size(file) > 8192)
        throw Error(ErrorCode::not_found, "CTP connection is unavailable");
    std::ifstream input(file);
    const std::string contents{std::istreambuf_iterator<char>(input), {}};
    const auto document = parse_json(contents, 8192);
    if (document.size() != (document.contains("trading") ? 9U : 8U) || document.at("id") != id)
        throw std::invalid_argument("invalid CTP connection storage");
    CtpConnection connection{
        document.at("id"),
        document.at("name"),
        document.at("broker_id"),
        document.at("user_id"),
        document.at("app_id"),
        document.at("trade_front"),
        document.at("market_front"),
        document.at("revision"),
        {}
    };
    if (document.contains("trading")) {
        const auto& target = document.at("trading");
        require_fields(target, {"node", "service"});
        connection.trading = TradingBinding{target.at("node"), target.at("service")};
        validate_id(connection.trading->node);
        validate_id(connection.trading->service);
    }
    if (connection.revision.empty())
        throw std::invalid_argument("invalid CTP connection storage");
    validate(connection);
    return connection;
}

Json CtpConnections::snapshot() const
{
    safe(directory_);
    std::error_code missing;
    const auto time = fs::last_write_time(directory_, missing);
    if (missing)
        return Json::array();
    // Saves and removals replace files by rename, which updates the directory time.
    if (!cached_ || time != cached_time_) {
        cached_ = read_all();
        // A directory changed this recently is read again next time.
        cached_time_ = directory_settled(time) ? time : fs::file_time_type::min();
    }
    return *cached_;
}

Json CtpConnections::read_all() const
{
    Json result = Json::array();
    for (const auto& entry : fs::directory_iterator(directory_)) {
        if (entry.path().extension() != ".json")
            continue;
        if (result.size() >= 64)
            break;
        const auto id = entry.path().stem().string();
        try {
            auto item = encode(get(id));
            result.push_back(std::move(item));
        } catch (const std::exception&) {
            // Keep the file for inspection; the rest of the Terminal stays usable.
            result.push_back({{"id", id}, {"name", id}, {"error", "unreadable"}});
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.at("name") < right.at("name");
    });
    return result;
}

void CtpConnections::save(CtpConnection connection, const std::string& expected)
{
    const auto file = path(connection.id);
    validate(connection);
    safe(directory_.parent_path());
    create_directories_durably(directory_);
    fs::permissions(directory_, fs::perms::owner_all);
    FileLock lock(directory_, "connections.lock");
    const auto current = fs::exists(file) ? std::optional{get(connection.id)} : std::nullopt;
    if ((current ? current->revision : std::string{}) != expected)
        throw Error(ErrorCode::conflict, "CTP connection changed; inspect again");
    if (current && current->trading) {
        if (connection.broker_id != current->broker_id || connection.user_id != current->user_id
            || connection.app_id != current->app_id
            || connection.trade_front != current->trade_front)
            throw Error(
                ErrorCode::conflict,
                "this account already trades; its counter details are fixed");
        connection.trading = current->trading;
    } else if (connection.trading)
        throw std::invalid_argument("bind the trading instance through account creation");
    if (!fs::exists(file) && read_all().size() >= 64)
        throw std::invalid_argument("too many CTP connections");
    if (current && remembered_ && credential_account(*current) != credential_account(connection))
        remembered_->erase(credential_account(*current));
    connection.revision = unique_process_id();
    replace_file_durably(file, encode(connection).dump(), true);
    cached_.reset();
    // The first account supplies market data without a further step.
    if (!market())
        replace_file_durably(directory_ / "market", connection.id, true);
}

void CtpConnections::bind_trading(
    const std::string& id,
    const std::string& expected,
    TradingBinding target)
{
    validate_id(target.node);
    validate_id(target.service);
    FileLock lock(directory_, "connections.lock");
    auto connection = get(id);
    if (connection.revision != expected)
        throw Error(ErrorCode::conflict, "CTP connection changed; inspect again");
    if (connection.trading) {
        if (*connection.trading != target)
            throw Error(
                ErrorCode::conflict,
                "trading account is already bound to another service instance");
        return;
    }
    for (const auto& item : read_all()) {
        if (item.contains("trading") && item.at("trading").at("node") == target.node
            && item.at("trading").at("service") == target.service)
            throw Error(
                ErrorCode::conflict,
                "trading service instance is already bound to another account");
    }
    connection.trading = std::move(target);
    connection.revision = unique_process_id();
    replace_file_durably(path(id), encode(connection).dump(), true);
    cached_.reset();
}

std::optional<CtpConnection> CtpConnections::market() const
{
    safe(directory_);
    const auto file = directory_ / "market";
    safe(file);
    if (!fs::is_regular_file(file) || fs::file_size(file) > 64)
        return std::nullopt;
    std::ifstream input(file);
    std::string id;
    input >> id;
    try {
        return get(id);
    } catch (const std::exception&) {
        return std::nullopt; // Removed or unreadable: no market data account.
    }
}

void CtpConnections::select_market(const std::string& id)
{
    static_cast<void>(get(id));
    FileLock lock(directory_, "connections.lock");
    replace_file_durably(directory_ / "market", id, true);
}

void CtpConnections::remove(const std::string& id, const std::string& expected)
{
    const auto file = path(id);
    FileLock lock(directory_, "connections.lock");
    const auto connection = get(id);
    if (connection.revision != expected)
        throw Error(ErrorCode::conflict, "CTP connection changed; inspect again");
    if (connection.trading)
        throw Error(
            ErrorCode::conflict,
            "account has a deployed trading instance; preserve its binding");
    if (remembered_)
        remembered_->erase(credential_account(connection));
    fs::remove(file);
    sync_directory(directory_);
    cached_.reset();
}

} // namespace asterion::terminal
