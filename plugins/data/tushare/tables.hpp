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
};
std::span<const TableDefinition> table_definitions();
using TableRow = std::vector<std::optional<std::string>>;
// Market-wide tables, one partition per read: futures.contracts from fut_basic,
// futures.calendar from trade_cal and futures.daily from fut_daily.
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
  Answer ask(const char* interface, Json parameters, const char* fields, std::size_t limit,
             const std::string& partition, std::stop_token stop);
  const std::vector<Listing>& catalogue(const std::string& venue, std::stop_token stop);
  std::string token_;
  Post post_;
  // fut_basic by venue: dated contracts only, kept for the life of the reader.
  std::map<std::string, std::vector<Listing>> catalogues_;
};
} // namespace asterion::tushare
