#pragma once
#include <asterion/domain/history_store.hpp>
#include <asterion/v1/data.pb.h>
#include <filesystem>
namespace asterion::history_files {
class Archive final : public HistoryStorePort {
public:
  explicit Archive(std::filesystem::path root);
  PluginDescriptor descriptor() const override;
  void start() override {}
  void stop() noexcept override {}
  std::vector<HistoryDataset> datasets(const HistoryFilter&) const override;
  std::filesystem::path directory(const HistoryIdentity&, const std::string& source,
                                  unsigned interval, const std::string& acquisition) const;
  void publish(const data::v1::HistoryRecord&);
  data::v1::HistoryRecord get(const std::string& id) const;

private:
  std::filesystem::path root_;
};
} // namespace asterion::history_files
