#include "history_coverage.hpp"
#include "history_minutes.hpp"
#include "table_series.hpp"
#include <asterion/protocol/data.hpp>
namespace asterion::data {
MinuteCoverage minute_coverage(const history_files::TableStore& tables,
                               const data::v1::DailyTable& daily, const std::string& contract,
                               const std::set<std::string>& minute_days) {
  MinuteCoverage result;
  if (minute_days.empty())
    return result;
  // This compares whole trading days; it is no proof of intraday completeness.
  auto days = traded_days(tables, daily, contract, *minute_days.begin(), *minute_days.rbegin());
  result.compared_first = std::move(days.first);
  result.compared_last = std::move(days.last);
  result.traded = static_cast<unsigned>(days.traded.size());
  for (auto& day : days.traded)
    if (!minute_days.contains(day))
      result.uncovered.push_back(std::move(day));
  return result;
}
data::v1::HistoryCoverages history_coverage(const history_files::Archive& archive,
                                            const history_files::TableStore& tables,
                                            const data::v1::HistoryCoverageQuery& query) {
  protocol::validate_message(query);
  protocol::validate_daily_table(query.daily());
  const auto& filter = query.filter();
  data::v1::HistoryCoverages result;
  for (const auto& item : archive.datasets(
           {filter.venue(), filter.product(), filter.contract_id(), filter.source()})) {
    const auto record = archive.get(item.id);
    if (!record.has_minute_result())
      continue;
    if (result.items_size() >= 10000)
      throw std::invalid_argument("historical archive query exceeds limit; narrow filters");
    const auto listed = history_files::minute_trading_days(
        std::filesystem::path(std::u8string(record.minute_result().directory().begin(),
                                            record.minute_result().directory().end())),
        item.id);
    const std::set<std::string> days(listed.begin(), listed.end());
    auto* row = result.add_items();
    row->set_contract_id(item.contract.key());
    row->set_minute_dataset_id(item.id);
    row->set_minute_source(item.source);
    row->set_interval_minutes(item.interval_minutes);
    row->set_minute_days(static_cast<unsigned>(days.size()));
    if (days.empty())
      continue;
    row->set_minute_first(*days.begin());
    row->set_minute_last(*days.rbegin());
    const auto coverage = minute_coverage(tables, query.daily(), item.contract.key(), days);
    row->set_compared_first(coverage.compared_first);
    row->set_compared_last(coverage.compared_last);
    row->set_traded_days(coverage.traded);
    row->set_uncovered(static_cast<unsigned>(coverage.uncovered.size()));
    for (std::size_t i = 0; i < coverage.uncovered.size() && i < 50; ++i)
      row->add_uncovered_days(coverage.uncovered[i]);
  }
  return result;
}
} // namespace asterion::data
