#pragma once
#include "data_store.hpp"
#include "history_minutes.hpp"
#include "history_daily.hpp"
namespace asterion::test {
template <class Input>
data::v1::DownloadAuthorization authorize_download(data::Store& store,
                                                   const std::string& task_instance,
                                                   const std::string& id, const Input& input) {
  data::v1::DownloadAuthorizationRequest request;
  request.set_task_instance(task_instance);
  request.set_task_id(id);
  request.set_credential("fixture");
  if constexpr (std::is_same_v<Input, data::v1::MinuteDownload>)
    *request.mutable_minutes() = input;
  else
    *request.mutable_daily() = input;
  return store.authorize(store.verify_authorization(request));
}
inline data::v1::DownloadDirectory allocate_download(data::Store& store,
                                                     data::v1::DownloadAllocation& allocation) {
  const auto authorized =
      allocation.has_minutes()
          ? authorize_download(store, allocation.identity().task_instance(),
                               allocation.identity().task_id(), allocation.minutes())
          : authorize_download(store, allocation.identity().task_instance(),
                               allocation.identity().task_id(), allocation.daily());
  allocation.set_authorization_id(authorized.id());
  return store.allocate(store.verify_allocation(allocation));
}
// Fixture providers publish real verified versions into an isolated data store.
// Task execution and publication ordering are covered by the process tests.
template <class Input, class Provider>
data::v1::HistoryRecord publish_download(data::Store& store, const std::string& id,
                                         const Input& input, Provider& provider) {
  data::v1::DownloadAllocation allocation;
  auto* identity = allocation.mutable_identity();
  identity->set_data_instance(store.instance());
  identity->set_task_instance("task");
  identity->set_task_id(id);
  identity->set_attempt(1);
  if constexpr (std::is_same_v<Input, data::v1::MinuteDownload>)
    *allocation.mutable_minutes() = input;
  else
    *allocation.mutable_daily() = input;
  const auto directory = allocate_download(store, allocation).directory();
  data::v1::DownloadPreparation preparation;
  *preparation.mutable_identity() = allocation.identity();
  auto* record = preparation.mutable_record();
  record->set_version(1);
  if constexpr (std::is_same_v<Input, data::v1::MinuteDownload>) {
    history_files::download_minutes(provider, history_files::minute_range(input), directory);
    *record->mutable_minutes() = input;
    *record->mutable_minute_result() = history_files::minute_result(directory);
  } else {
    history_files::download_daily(provider, history_files::daily_range(input), directory);
    *record->mutable_daily() = input;
    *record->mutable_daily_result() = history_files::daily_result(directory);
  }
  const auto prepared = store.prepare(store.verify_download(preparation));
  data::v1::DownloadPublication decision;
  *decision.mutable_identity() = prepared.identity();
  decision.set_candidate_digest(prepared.candidate_digest());
  decision.set_publication_id(id);
  return store.publish(store.verify_publication(decision)).record();
}
} // namespace asterion::test
