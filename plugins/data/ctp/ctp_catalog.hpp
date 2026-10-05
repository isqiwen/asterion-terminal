#pragma once
#include <asterion/domain/market.hpp>
#include <chrono>
#include <filesystem>
#include <stop_token>
#include <memory>
#include <optional>
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
// One SDK owner constructs, advances and destroys this read-only session.
// poll never waits for a response. Destruction releases the SDK and can block.
// timeout bounds each connection step and silence between instrument responses.
class CatalogQuery {
public:
  CatalogQuery(const std::filesystem::path& library, const std::filesystem::path& flow,
               CatalogConfiguration configuration,
               std::chrono::milliseconds timeout = std::chrono::seconds(30));
  ~CatalogQuery();
  std::optional<Catalog> poll(std::stop_token stop = {});

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::ctp
