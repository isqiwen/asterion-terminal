#pragma once
#include "task_store.hpp"

namespace asterion::data {

// What Data's acquisition task kinds mean to its task store.
const tasks::TaskDomain& task_domain();

// Table updates whose fixed input or published result holds a daily-table snapshot.
std::vector<std::string> table_snapshot_holds(const tasks::Store&, const std::string& snapshot);

// The candidate a verified minute-download completion publishes.
proto::DownloadPreparation
download_preparation(const tasks::Store::Completion&, const std::string& instance);
// Transfers a verified completion to its publication step once Data prepared the candidate.
[[nodiscard]] tasks::Store::Change
prepare_publication(tasks::Store&, tasks::Store::Completion, const proto::PreparedDownload&);
[[nodiscard]] tasks::Store::Change
confirm_publication(tasks::Store&, const proto::PublishedDownload&);

} // namespace asterion::data
