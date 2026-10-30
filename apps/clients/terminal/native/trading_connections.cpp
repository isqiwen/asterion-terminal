#include "trading_connections.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <fstream>
#include <asterion/kernel/process/child.hpp>
#include <algorithm>

namespace asterion::terminal {
namespace fs = std::filesystem;

namespace {

void validate(const Json& value)
{
    require_fields(value, {"id", "name", "host", "port", "instance", "fingerprint", "revision"});
    validate_id(value.at("id").get<std::string>());
    validate_id(value.at("instance").get<std::string>());
    for (const auto* field : {"name", "host"}) {
        const auto text = value.at(field).get<std::string>();
        if (text.empty() || text.size() > 253 || std::ranges::any_of(text, [](unsigned char c) {
                return c < 32 || c == 127;
            }))
            throw std::invalid_argument("invalid trading connection name or address");
    }
    const auto& port = value.at("port");
    if (!port.is_number_unsigned() || port.get<unsigned long long>() < 1
        || port.get<unsigned long long>() > 65535)
        throw std::invalid_argument("trading connection port must be in 1-65535");
    if (ipc::normalize_fingerprint(value.at("fingerprint").get<std::string>())
        != value.at("fingerprint").get<std::string>())
        throw std::invalid_argument("invalid trading server fingerprint");
}

} // namespace

fs::path TradingConnections::path(const std::string& id) const
{
    validate_id(id);
    auto result = data_ / "trading-connections" / (id + ".json");
    if (!result.is_absolute() || fs::is_symlink(result) || fs::is_symlink(result.parent_path()))
        throw std::invalid_argument("invalid trading connection storage");
    return result;
}

Json TradingConnections::get(const std::string& id) const
{
    const auto file = path(id);
    if (!fs::is_regular_file(file) || fs::file_size(file) > 8192)
        throw Error(ErrorCode::not_found, "trading connection is unavailable");
    std::ifstream input(file);
    auto value = parse_json(std::string{std::istreambuf_iterator<char>(input), {}}, 8192);
    validate(value);
    if (value.at("id") != id)
        throw std::invalid_argument("invalid trading connection storage");
    return value;
}

Json TradingConnections::snapshot() const
{
    Json result = Json::array();
    const auto directory = path("check").parent_path();
    if (!fs::exists(directory))
        return result;
    for (const auto& item : fs::directory_iterator(directory)) {
        if (item.path().extension() != ".json")
            continue;
        const auto id = item.path().stem().string();
        try {
            result.push_back(get(id));
        } catch (const std::exception&) {
            result.push_back({{"id", id}, {"name", id}, {"error", "unreadable"}});
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.at("name") < b.at("name");
    });
    return result;
}

void TradingConnections::save(Json value, const std::string& revision)
{
    value["fingerprint"] = ipc::normalize_fingerprint(value.at("fingerprint"));
    value["revision"] = revision;
    validate(value);
    const auto file = path(value.at("id"));
    create_directories_durably(file.parent_path());
    fs::permissions(file.parent_path(), fs::perms::owner_all, fs::perm_options::replace);
    FileLock lock(file.parent_path(), "connections.lock");
    const auto current = fs::exists(file) ? get(value.at("id")) : Json(nullptr);
    if ((current.is_null() ? "" : current.at("revision").get<std::string>()) != revision)
        throw Error(ErrorCode::conflict, "trading connection changed; inspect again");
    // Two labels must not create two independent bindings to the same instance.
    for (const auto& other : snapshot()) {
        if (other.contains("error") || other.at("id") == value.at("id"))
            continue;
        if (other.at("host") == value.at("host") && other.at("port") == value.at("port"))
            throw Error(ErrorCode::conflict, "trading endpoint already has a connection");
    }
    value["revision"] = unique_process_id();
    replace_file_durably(file, value.dump(), true);
}

void TradingConnections::remove(const std::string& id, const std::string& revision)
{
    FileLock lock(path(id).parent_path(), "connections.lock");
    if (get(id).at("revision") != revision)
        throw Error(ErrorCode::conflict, "trading connection changed; inspect again");
    fs::remove(path(id));
    sync_directory(path(id).parent_path());
}

std::string TradingConnections::fingerprint() const
{
    const auto directory = data_ / "tls";
    const auto certificate = directory / "client.crt", key = directory / "client.key";
    if (fs::is_symlink(directory) || fs::is_symlink(certificate) || fs::is_symlink(key))
        throw std::invalid_argument("invalid Terminal TLS identity storage");
    if (!fs::exists(certificate) && !fs::exists(key))
        return {};
    if (!fs::is_regular_file(certificate) || !fs::is_regular_file(key))
        throw std::invalid_argument(
            "Terminal TLS identity is incomplete; preserve and inspect its files");
    return ipc::certificate_fingerprint(certificate.string());
}

void TradingConnections::prepare_identity()
{
    create_directories_durably(data_);
    FileLock lock(data_, "trading-identity.lock");
    if (!fingerprint().empty())
        return;
    // Key and certificate appear together: both are written into a staging
    // directory that is then renamed to tls/. A run that ended between them
    // left only its staging directory, removed here.
    for (const auto& entry : fs::directory_iterator(data_))
        if (entry.path().filename().string().starts_with(".tls-"))
            fs::remove_all(entry.path());
    const auto staging = data_ / (".tls-" + unique_process_id());
    create_directories_durably(staging);
    fs::permissions(staging, fs::perms::owner_all, fs::perm_options::replace);
    ipc::create_identity(staging / "client.crt", staging / "client.key", "Asterion Terminal");
    fs::rename(staging, data_ / "tls");
    sync_directory(data_);
}

TradingEndpoint TradingConnections::endpoint(const std::string& id) const
{
    const auto value = get(id);
    if (fingerprint().empty())
        throw std::invalid_argument("prepare the Terminal certificate first");
    return {
        value.at("host"),
        value.at("instance"),
        value.at("port").get<std::uint16_t>(),
        {(data_ / "tls/client.crt").string(),
            (data_ / "tls/client.key").string(),
            {value.at("fingerprint")}}
    };
}

} // namespace asterion::terminal
