#pragma once
#include <asterion/domain/history_identity.hpp>
#include <vector>
namespace asterion {
struct HistoryDataset {
  std::string id;
  HistoryIdentity contract;
  std::string source, revision, begin, end;
  unsigned interval_minutes = 0;
  std::uint64_t rows = 0;
};
struct HistoryFilter {
  std::string venue, product, contract_id, source;
};
class HistoryStorePort {
public:
  virtual ~HistoryStorePort() = default;
  virtual std::vector<HistoryDataset> datasets(const HistoryFilter&) const = 0;
};
} // namespace asterion
