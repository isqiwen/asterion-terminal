#pragma once
#include <asterion/domain/source_response.hpp>
#include <asterion/domain/history_tables.hpp>
namespace asterion::tushare {
inline constexpr auto daily_response_format = "tushare.fut_daily.raw.v1";
// Strict successful SHFE daily-response allowlist. No redaction or projection:
// rejected shapes have no raw evidence. This never receives a credential.
bool allow_daily_response(const std::string& body, const std::string& venue,
                          const std::string& day) noexcept;
struct DailyResponseDifference {
  std::string kind, key, field;
  std::optional<std::string> raw, expected, actual;
};
struct DailyResponseReview {
  std::string status = "unknown", reason = "raw_missing";
  std::size_t response_fields = 0, response_rows = 0, normalized_rows = 0, dated_rows = 0,
              matched_rows = 0, compared_cells = 0;
  std::vector<std::string> skipped_keys, unknown_keys;
  std::vector<DailyResponseDifference> differences;
};
// Reconciles the complete stored partition, never a displayed page. Does not
// query, persist, repair, round, infer a century or validate the source catalogue.
DailyResponseReview review_daily_response(const std::optional<SourceResponse>&,
                                          const TablePartition&);
} // namespace asterion::tushare
