#pragma once
#include "ctp_catalog.hpp"
#include "ctp_feed.hpp"
#include <filesystem>
#include <optional>

namespace asterion::market_data {

// A recording node logs in on its own: the counter's addresses and account
// come from the node's config/market-recording.json, the password and the
// authentication code from systemd credentials (ImportCredential=asterion.ctp.*
// in the Agent's unit decrypts /etc/credstore.encrypted/asterion.ctp.password
// and asterion.ctp.auth_code into $CREDENTIALS_DIRECTORY). Without the file a
// node does not log in by itself; a file without its credentials is refused.
struct Unattended
{
    ctp::Configuration market;
    ctp::CatalogConfiguration catalog;
};

std::optional<Unattended> load_unattended(
    const std::filesystem::path& configuration,
    const std::optional<std::filesystem::path>& credentials);

} // namespace asterion::market_data
