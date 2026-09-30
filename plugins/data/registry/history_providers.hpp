#pragma once
#include <asterion/domain/historical_bars.hpp>
#include <asterion/domain/daily_bars.hpp>
#include <memory>
namespace asterion::history_providers {
struct ConnectionSchema {
  std::string credential_label_en, credential_label_zh;
  bool credential_required = false, remember_allowed = false;
  unsigned credential_max_length = 0, requests_per_minute_default = 0, requests_per_minute_max = 0;
};
struct ConnectionCheck {
  std::string scope;
  unsigned state;
};
struct Source {
  std::string id, name, plugin_id;
  HistorySemantics semantics;
  std::vector<std::string> venues;
  std::vector<unsigned> intervals;
  unsigned max_requests_per_minute;
  bool credential_required;
  std::optional<ConnectionSchema> connection;
};
std::vector<Source> sources();
std::string artifact(const std::string& source);
std::vector<ConnectionCheck> verify_connection(const std::string& source,
                                               const std::string& credential,
                                               std::stop_token stop = {});
void validate_request(const std::string& source, const HistoryIdentity&, unsigned interval,
                      unsigned requests_per_minute);
std::unique_ptr<HistoricalBarPort> minutes(const std::string& source,
                                           const std::string& credential);
std::unique_ptr<HistoricalDailyPort> daily(const std::string& source,
                                           const std::string& credential);
std::vector<HistoryListing> catalog(const std::string& source, const std::string& credential,
                                    const std::string& venue, const std::string& product,
                                    std::stop_token = {});
} // namespace asterion::history_providers
