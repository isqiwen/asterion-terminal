#include "history_coverage.hpp"
#include "history_daily.hpp"
#include "history_minutes.hpp"
#include <map>
#include <set>
namespace asterion::tasks {
namespace {
std::filesystem::path directory(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
} // namespace
data::v1::HistoryCoverages history_coverage(const history_files::Archive& archive,
                                            const data::v1::HistoryFilter& filter) {
  struct Days {
    std::set<std::string> minute, daily;
  };
  std::map<std::string, Days> contracts;
  const HistoryStorePort& store = archive;
  for (const auto& item :
       store.datasets({filter.venue(), filter.product(), filter.contract_id(), filter.source()})) {
    const auto record = archive.get(item.id);
    auto& days = contracts[item.contract.key()];
    if (record.has_minute_result())
      for (auto& day :
           history_files::minute_trading_days(directory(record.minute_result().directory())))
        days.minute.insert(std::move(day));
    else if (record.has_daily_result())
      for (auto& day :
           history_files::daily_trading_days(directory(record.daily_result().directory())))
        days.daily.insert(std::move(day));
  }
  data::v1::HistoryCoverages result;
  for (const auto& [contract, days] : contracts) {
    auto* row = result.add_items();
    row->set_contract_id(contract);
    row->set_minute_days(static_cast<unsigned>(days.minute.size()));
    row->set_daily_days(static_cast<unsigned>(days.daily.size()));
    if (!days.minute.empty()) {
      row->set_minute_first(*days.minute.begin());
      row->set_minute_last(*days.minute.rbegin());
    }
    if (!days.daily.empty()) {
      row->set_daily_first(*days.daily.begin());
      row->set_daily_last(*days.daily.rbegin());
    }
    if (days.minute.empty() || days.daily.empty())
      continue;
    unsigned uncovered = 0;
    for (auto day = days.daily.lower_bound(*days.minute.begin());
         day != days.daily.end() && *day <= *days.minute.rbegin(); ++day)
      if (!days.minute.contains(*day) && ++uncovered <= 50)
        row->add_uncovered_days(*day);
    row->set_uncovered(uncovered);
  }
  return result;
}
} // namespace asterion::tasks
