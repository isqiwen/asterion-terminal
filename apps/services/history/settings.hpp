#pragma once
#include "ctp_catalog.hpp"
#include "ctp_feed.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <map>
#include <asterion/system/ipc/tcp_channel.hpp>

namespace asterion::history {

// Read-only installation settings. Provider secrets come only from systemd's
// credential directory. TLS identities and client permissions belong to this host.
struct Settings
{
    std::filesystem::path directory, ctp_library, ctp_catalog_library;
    std::string bind;
    std::uint16_t port = 0;
    ipc::TlsIdentity tls;
    std::map<std::string, bool> clients; // Verified fingerprint -> acquisition management.
    std::vector<std::string> exchanges;
    std::string tushare_token;
    unsigned tushare_requests_per_minute = 0;
    // Recorded tick days kept; minutes and observation segments are kept for good.
    ctp::Configuration market;
    ctp::CatalogConfiguration catalog;
};

Settings load_settings(
    const std::filesystem::path& root,
    const std::optional<std::filesystem::path>& credentials);

} // namespace asterion::history
