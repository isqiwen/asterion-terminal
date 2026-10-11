#pragma once
#include "tushare.hpp"
#include <span>
namespace asterion::tushare {
// The vendor answered, but the partition cannot be used. The text is made of
// our own words and identifiers from the market data, so it may be shown.
class TableError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};
struct TableDefinition {
  const char *table, *source;
  unsigned schema;
  std::span<const char* const> venues;
};
std::span<const TableDefinition> table_definitions();
using TableRow = std::vector<std::optional<std::string>>;
// Market-wide tables, one partition per read: futures.contracts from fut_basic,
// futures.calendar from trade_cal, futures.daily from fut_daily,
// futures.settlement from fut_settle, futures.receipts from fut_wsr and
// futures.rankings from fut_holding.
class Tables final {
public:
  explicit Tables(std::string token, Post post = https_transport());
  ~Tables();
  // Cells in the order of `columns`; no rows when the vendor has none yet.
  std::vector<TableRow> read(std::string_view table, std::span<const std::string> columns,
                             const std::string& venue, const std::string& period,
                             std::stop_token stop);

private:
  struct Listing;
  struct Answer;
  Answer request(const char* interface, const Json& parameters, const char* fields,
                 const std::string& partition, std::stop_token stop);
  // One request for a table the vendor's limit of rows cannot cut short: an
  // answer that reaches the limit is refused.
  Answer ask(const char* interface, const Json& parameters, const char* fields, std::size_t limit,
             const std::string& partition, std::stop_token stop);
  // A table read `size` rows at a time, for as long as pages come back full.
  Answer pages(const char* interface, Json parameters, const char* fields, std::size_t size,
               const std::string& partition, std::stop_token stop);
  const std::vector<Listing>& catalogue(const std::string& venue, std::stop_token stop);
  // The contract a vendor code names on a day.
  const Listing& listed(const std::string& venue, const std::string& code,
                        const std::string& period, std::stop_token stop);
  // Whether the exchange lists futures of this product.
  bool lists(const std::string& venue, const std::string& product, std::stop_token stop);
  // The vendor answers for SHFE with the contracts of INE, its subsidiary,
  // among them, and has nothing under INE itself. One answer serves both
  // partitions of a day; each keeps the rows of its own products.
  const Answer& shanghai(const char* interface, const char* fields, std::size_t size,
                         const std::string& period, const std::string& partition,
                         std::stop_token stop);
  using Columns = std::span<const std::string>;
  std::vector<TableRow> calendar(Columns, const std::string& venue, const std::string& period,
                                 std::stop_token);
  std::vector<TableRow> daily(Columns, const std::string& venue, const std::string& period,
                              std::stop_token);
  std::vector<TableRow> settlement(Columns, const std::string& venue, const std::string& period,
                                   std::stop_token);
  std::vector<TableRow> receipts(Columns, const std::string& venue, const std::string& period,
                                 std::stop_token);
  std::vector<TableRow> rankings(Columns, const std::string& venue, const std::string& period,
                                 std::stop_token);
  std::string token_;
  Post post_;
  // fut_basic by venue: dated contracts only, kept for the life of the reader.
  std::map<std::string, std::vector<Listing>> catalogues_;
  struct Shared;
  std::map<std::string, std::unique_ptr<Shared>> shanghai_; // by interface: the last day asked
  std::set<std::string> reported_;
};
} // namespace asterion::tushare
