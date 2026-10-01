#pragma once
#include <asterion/domain/market.hpp>
#include <chrono>
#include <filesystem>
#include <stop_token>
#include <vector>
namespace asterion::ctp {
// A separate read-only TraderApi session; never reuse the execution plugin.
struct CatalogConfiguration {
  std::string front, broker, user, password, app_id, auth_code;
};
struct CatalogContract {
  InstrumentId instrument;
  std::string name, product, expiry;
  std::string contract_id = {};
  int multiplier = 0;
  Decimal price_tick;
};
struct Catalog {
  std::string trading_day;
  std::vector<CatalogContract> contracts;
};
// Blocking provider I/O: callers must run this outside UI/global command locks.
// A failure never returns a partial catalog. SDK Release can outlive the deadline.
Catalog read_catalog(const std::filesystem::path& library, const std::filesystem::path& flow,
                     CatalogConfiguration configuration, std::stop_token stop = {},
                     std::chrono::milliseconds timeout = std::chrono::seconds(30));
} // namespace asterion::ctp
