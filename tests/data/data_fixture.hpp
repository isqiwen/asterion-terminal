#pragma once
#include "data_store.hpp"
#include "history_minutes.hpp"

namespace asterion::test {

inline data::proto::DownloadAuthorization authorize_download(
    data::Store& store,
    const std::string& task_instance,
    const std::string& id,
    const data::proto::MinuteDownload& input)
{
    data::proto::DownloadAuthorizationRequest request;
    request.set_task_instance(task_instance);
    request.set_task_id(id);
    request.set_credential("fixture");
    *request.mutable_minutes() = input;
    return store.authorize(store.verify_authorization(request));
}

inline data::proto::DownloadDirectory
allocate_download(data::Store& store, data::proto::DownloadAllocation& allocation)
{
    const auto authorized = authorize_download(
        store,
        allocation.identity().task_instance(),
        allocation.identity().task_id(),
        allocation.minutes());
    allocation.set_authorization_id(authorized.id());
    return store.allocate(store.verify_allocation(allocation));
}

// Fixture providers publish real verified versions into an isolated data store.
// Task execution and publication ordering are covered by the process tests.
template <class Provider>
data::proto::HistoryRecord publish_download(
    data::Store& store,
    const std::string& id,
    const data::proto::MinuteDownload& input,
    Provider& provider)
{
    data::proto::DownloadAllocation allocation;
    auto* identity = allocation.mutable_identity();
    identity->set_data_instance(store.instance());
    identity->set_task_instance(store.instance());
    identity->set_task_id(id);
    identity->set_attempt(1);
    *allocation.mutable_minutes() = input;
    const auto directory = allocate_download(store, allocation).directory();
    data::proto::DownloadPreparation preparation;
    *preparation.mutable_identity() = allocation.identity();
    auto* record = preparation.mutable_record();
    history_files::download_minutes(provider, history_files::minute_range(input), directory);
    *record->mutable_minutes() = input;
    *record->mutable_minute_result() = history_files::minute_result(directory);
    const auto prepared = store.prepare(store.verify_download(preparation));
    data::proto::DownloadPublication decision;
    *decision.mutable_identity() = prepared.identity();
    decision.set_candidate_digest(prepared.candidate_digest());
    decision.set_publication_id(id);
    return store.publish(store.verify_publication(decision)).record();
}

} // namespace asterion::test
