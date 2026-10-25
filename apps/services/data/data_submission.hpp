#pragma once
#include "task_store.hpp"

namespace asterion::tasks {

// Data acquisition submissions: material requests and authorized downloads.
Store::Submission task_submission(const Store& store, data::proto::MaterialInput input);
Store::Submission
task_submission(const Store& store, const data::proto::DownloadAuthorization& authorization);

} // namespace asterion::tasks
