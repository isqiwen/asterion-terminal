#pragma once
#include <asterion/domain/history_store.hpp>
#include <asterion/v1/data.pb.h>
#include <filesystem>
namespace asterion::history_files {
class Archive final {
public:
  enum class Access { writer, read_only };
  explicit Archive(std::filesystem::path root, Access access = Access::writer);
  void start() {}
  void stop() noexcept {}
  // The versions matching a filter. One another version published that this
  // one does not read (a per-contract daily download) stays where it is and
  // is counted, so that it does not take the others with it.
  struct Listing {
    std::vector<HistoryDataset> items;
    unsigned unsupported = 0;
  };
  Listing datasets(const HistoryFilter&) const;
  std::filesystem::path directory(const HistoryIdentity&, const std::string& source,
                                  unsigned interval, const std::string& acquisition) const;
  // The data owner verifies files on its file pool before retaining prepared
  // evidence. Publication only exposes that immutable, already verified record.
  void publish_verified(const data::v1::HistoryRecord&);
  data::v1::HistoryRecord get(const std::string& id) const;
  void save_named_dataset(const data::v1::NamedDataset&);
  data::v1::NamedDataset named_dataset(const std::string&) const;
  bool has_named_dataset(const std::string&) const;
  data::v1::NamedDatasets named_datasets() const;

private:
  std::filesystem::path root_;
  Access access_;
  void writable() const;
};
} // namespace asterion::history_files
