#pragma once
#include <asterion/domain/history_store.hpp>
#include <asterion/v1/data.pb.h>
#include <filesystem>
namespace asterion::history_files {
class Archive final : public HistoryStorePort {
public:
  enum class Access { writer, read_only };
  explicit Archive(std::filesystem::path root, Access access = Access::writer);
  PluginDescriptor descriptor() const override;
  void start() override {}
  void stop() noexcept override {}
  std::vector<HistoryDataset> datasets(const HistoryFilter&) const override;
  std::filesystem::path directory(const HistoryIdentity&, const std::string& source,
                                  unsigned interval, const std::string& acquisition) const;
  void publish(const data::v1::HistoryRecord&);
  data::v1::HistoryRecord get(const std::string& id) const;
  void save_research_dataset(const data::v1::ResearchDataset&);
  data::v1::ResearchDataset research_dataset(const std::string&) const;
  data::v1::ResearchDatasets research_datasets() const;

private:
  std::filesystem::path root_;
  Access access_;
  void writable() const;
};
} // namespace asterion::history_files
