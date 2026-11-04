#include "market_endpoint.hpp"
#include <asterion/system/ipc/tcp_channel.hpp>
#include <asterion/base/error.hpp>
#include <asterion/system/durable_file.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <asterion/futures/market.hpp>

namespace asterion::terminal {
namespace fs = std::filesystem;

namespace {

void validate(const std::string& host, unsigned port)
{
    if (host.empty() || host.size() > 253
        || std::any_of(host.begin(), host.end(), [](unsigned char c) {
               return std::isspace(c) || std::iscntrl(c);
           }))
        throw std::invalid_argument("invalid market host");
    if (port < 1 || port > 65535)
        throw std::invalid_argument("invalid market port");
}

void validate_watchlist(const Json& value)
{
    if (!value.is_array() || value.size() > 50)
        throw std::invalid_argument("at most 50 watchlist contracts");
    std::set<InstrumentId> unique;
    for (const auto& item : value) {
        require_fields(item, {"exchange", "symbol"});
        InstrumentId id{
            item.at("exchange").get<std::string>(),
            item.at("symbol").get<std::string>()
        };
        id.validate();
        if (!unique.insert(id).second)
            throw std::invalid_argument("duplicate watchlist contract");
    }
}

} // namespace

Json MarketEndpoint::get() const
{
    if (!fs::exists(file_))
        return nullptr;
    try {
        if (fs::is_symlink(file_) || !fs::is_regular_file(file_) || fs::file_size(file_) > 16384)
            throw std::invalid_argument("not a small regular file");
        std::ifstream input(file_);
        const auto value =
            parse_json(std::string{std::istreambuf_iterator<char>(input), {}}, 16384);
        require_fields(value, {"host", "port", "fingerprint", "instruments"});
        if (!value.at("port").is_number_unsigned() || value.at("port").get<std::uint64_t>() > 65535)
            throw std::invalid_argument("invalid market port");
        const auto host = value.at("host").get<std::string>();
        const auto port = value.at("port").get<unsigned>();
        validate(host, port);
        if (ipc::normalize_fingerprint(value.at("fingerprint"))
            != value.at("fingerprint").get<std::string>())
            throw std::invalid_argument("invalid Market certificate fingerprint");
        validate_watchlist(value.at("instruments"));
        return value;
    } catch (const std::exception& error) {
        throw std::invalid_argument(
            "invalid market settings " + file_.string() + ": " + error.what());
    }
}

void MarketEndpoint::save(
    const std::string& host,
    unsigned port,
    const std::string& fingerprint) const
{
    validate(host, port);
    create_directories_durably(file_.parent_path());
    const auto normalized = ipc::normalize_fingerprint(fingerprint);
    const auto previous = get();
    const auto instruments = previous.is_null() ? Json::array() : previous.at("instruments");
    replace_file_durably(
        file_,
        Json{
            {"host", host},
            {"port", port},
            {"fingerprint", normalized},
            {"instruments", instruments}
        }.dump());
}

void MarketEndpoint::select(const Json& instruments) const
{
    validate_watchlist(instruments);
    auto value = get();
    if (value.is_null())
        throw std::invalid_argument("configure the market address first");
    value["instruments"] = instruments;
    replace_file_durably(file_, value.dump());
}

void MarketEndpoint::clear() const
{
    if (fs::exists(file_)) {
        fs::remove(file_);
        sync_directory(file_.parent_path());
    }
}

} // namespace asterion::terminal
