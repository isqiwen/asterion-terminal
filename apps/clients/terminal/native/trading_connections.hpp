#pragma once
#include <asterion/foundation/serialization.hpp>
#include <asterion/kernel/ipc/tcp_channel.hpp>
#include <filesystem>

namespace asterion::terminal {

struct TradingEndpoint
{
    std::string host, session;
    std::uint16_t port;
    ipc::TlsIdentity tls;
};

// Saved destinations contain no CTP credentials. The Terminal's private key
// stays in its own installation and is never included in a snapshot.
class TradingConnections
{
public:
    explicit TradingConnections(std::filesystem::path data)
        : data_(std::move(data))
    {}

    Json snapshot() const;
    Json get(const std::string& id) const;
    void save(Json value, const std::string& revision);
    void remove(const std::string& id, const std::string& revision);
    std::string fingerprint() const;
    void prepare_identity();
    TradingEndpoint endpoint(const std::string& id) const;

private:
    std::filesystem::path data_;
    std::filesystem::path path(const std::string& id) const;
};

} // namespace asterion::terminal
