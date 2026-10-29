#pragma once
#include "ctp_catalog.hpp"
#include "ctp_feed.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>

namespace asterion::recording {

// The recorder is installed as one directory, found from its own location
// (<root>/bin/asterion_recorder):
//
//   bin/     asterion_recorder
//   lib/     ctp-md.so, ctp-trader.so
//   config/  recorder.json, recorder-logging.json
//   logs/
//   data/    recordings, the catalog cache, CTP flow files
//
// config/recorder.json holds the rest (defaults in the recorder's own
// config/recorder.json in the repository):
//
//   {"bind": "0.0.0.0", "port": 7450,
//    "counter": {"market_front": "tcp://…", "trade_front": "tcp://…",
//                "broker": "…", "user": "…", "app_id": "…"}}
//
// The password and the authentication code are not in it: they come from
// systemd credentials (ImportCredential=asterion.ctp.* in the recorder's unit
// decrypts /etc/credstore.encrypted/asterion.ctp.password and
// asterion.ctp.auth_code into $CREDENTIALS_DIRECTORY). A missing directory,
// library or key, or a missing credential, is refused with its path.
struct Settings
{
    std::filesystem::path directory, ctp_library, ctp_catalog_library;
    std::string bind;
    std::uint16_t port = 0;
    ctp::Configuration market;
    ctp::CatalogConfiguration catalog;
};

Settings load_settings(
    const std::filesystem::path& root,
    const std::optional<std::filesystem::path>& credentials);

} // namespace asterion::recording
