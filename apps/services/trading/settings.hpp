#pragma once
#include <asterion/system/rpc_host.hpp>
#include <filesystem>
#include <asterion/base/serialization.hpp>
#include <optional>
#include <string>
#include "broker_execution.hpp"
#include "ctp_feed.hpp"

namespace asterion::trading {

// The trading service is installed as one directory, found from its own
// location (<root>/bin/asterion_trading), with one instance per CTP account:
//
//   bin/     asterion_trading
//   lib/     ctp-trader.so, ctp-md.so
//   config/  logging.json, tls/server.crt and server.key, accounts/<account>.json
//   logs/
//   data/    <account>/ the account's trading record; owners/ the execution leases
//
// config/accounts/<account>.json says where the instance listens and which
// Terminals may connect, by their certificate fingerprints:
//
//   {"bind": "0.0.0.0", "port": 7460, "terminals": ["<64 hex digits>"],
//    "market_front": "tcp://host:port", "environment": "simulation",
//    "strategy_memory_limit_mb": 2048, "broker": {"front": "tcp://host:port",
//    "broker_id": "9999", "user_id": "account", "app_id": "application"}}
//
// Connections are TLS with this host's certificate; only the listed Terminals
// are accepted (tcp_channel.hpp). A missing file, directory or key is refused
// with its path. Password and auth_code come only from CREDENTIALS_DIRECTORY,
// populated by systemd from each instance's encrypted credentials.
inline constexpr std::uint64_t default_strategy_memory_limit_mb = 2048;
inline constexpr std::uint64_t min_strategy_memory_limit_mb = 512;
inline constexpr std::uint64_t max_strategy_memory_limit_mb = 65536;

struct Settings
{
    std::string account;
    // The account's stable identity: its name, environment, broker and investor.
    Json identity;
    BrokerLogin login;
    ctp::Configuration market;
    service::Transport transport;
    std::filesystem::path journal, owners, ctp_library;
    std::optional<std::string> alert_webhook;
    std::uint64_t strategy_memory_limit_mb = default_strategy_memory_limit_mb;
};

Settings load_settings(const std::filesystem::path& root, const std::string& account);

} // namespace asterion::trading
