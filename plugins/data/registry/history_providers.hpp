#pragma once
#include <asterion/domain/source_response.hpp>
#include <asterion/domain/historical_bars.hpp>
#include <asterion/domain/history_tables.hpp>
#include <memory>
#include <functional>
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
using RequestBudget = std::function<void(std::stop_token)>;
// Returns a ready business port. Reads require a request budget callback. The native instance owns
// plugin start/stop.
std::unique_ptr<HistoricalBarPort> minutes(const std::string& source, const std::string& credential,
                                           RequestBudget budget = {});
// A market-wide table one installed plugin provides.
struct TableSource {
  std::string table;
  unsigned schema = 0;
  std::string source, plugin_id;
  std::vector<std::string> venues;
};
// Only tables whose column definition the plugin shares with this host.
std::vector<TableSource> table_sources();
// The library a table update pins.
std::string table_artifact(const std::string& plugin_id);
// The source answered but the partition cannot be used; what() says why.
class TableRejected final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};
class TableReader {
public:
  virtual ~TableReader() = default;
  virtual std::optional<SourceResponse> response() const { return {}; }
  // Rows of one partition in the table's column order; none when the source
  // returns no rows. An empty response does not establish historical coverage
  // or whether the source has published data. Other failures did not complete.
  virtual std::vector<TableSourceRow> read(const HistoryTable&, const std::string& venue,
                                           const std::string& period, std::stop_token) = 0;
};
// Source-specific retention policy; no credentials or requests are accepted.
void validate_response(const std::string& table, const std::string& venue,
                       const std::string& period, const SourceResponse&);
// One reader serves every table of the plugin; reads acquire the request budget.
std::unique_ptr<TableReader> tables(const std::string& plugin_id, const std::string& credential,
                                    RequestBudget budget = {});
} // namespace asterion::history_providers
