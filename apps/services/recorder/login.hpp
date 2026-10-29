#pragma once
#include "ctp_catalog.hpp"
#include "ctp_feed.hpp"
#include <filesystem>
#include <optional>

namespace asterion::recording {

// The recorder logs in by itself: the counter's addresses and account come
// from its configuration file, the password and the authentication code from
// systemd credentials (ImportCredential=asterion.ctp.* in the recorder's unit
// decrypts /etc/credstore.encrypted/asterion.ctp.password and
// asterion.ctp.auth_code into $CREDENTIALS_DIRECTORY). A missing or incomplete
// file, or a missing credential, is refused with its path.
struct Login
{
    ctp::Configuration market;
    ctp::CatalogConfiguration catalog;
};

Login load_login(
    const std::filesystem::path& configuration,
    const std::optional<std::filesystem::path>& credentials);

} // namespace asterion::recording
