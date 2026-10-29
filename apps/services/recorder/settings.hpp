#pragma once
#include "ctp_catalog.hpp"
#include "ctp_feed.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>

namespace asterion::recording {

// Everything the recorder runs with comes from one file, recorder.json
// (defaults in the repository's config/recorder.json):
//
//   {"directory": "/var/lib/asterion-recorder",
//    "ctp_library": "…/ctp-md.so", "ctp_catalog_library": "…/ctp-trader.so",
//    "bind": "0.0.0.0", "port": 7450,
//    "counter": {"market_front": "tcp://…", "trade_front": "tcp://…",
//                "broker": "…", "user": "…", "app_id": "…"}}
//
// The password and the authentication code are not in it: they come from
// systemd credentials (ImportCredential=asterion.ctp.* in the recorder's unit
// decrypts /etc/credstore.encrypted/asterion.ctp.password and
// asterion.ctp.auth_code into $CREDENTIALS_DIRECTORY). A missing or invalid
// key, or a missing credential, is refused with the file's path.
struct Settings
{
    std::filesystem::path directory, ctp_library, ctp_catalog_library;
    std::string bind;
    std::uint16_t port = 0;
    ctp::Configuration market;
    ctp::CatalogConfiguration catalog;
};

Settings load_settings(
    const std::filesystem::path& file,
    const std::optional<std::filesystem::path>& credentials);

} // namespace asterion::recording
