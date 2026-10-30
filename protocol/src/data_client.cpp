#include <asterion/protocol/data_client.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/material.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/rpc_host.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/kernel/process/artifact.hpp>

namespace asterion::protocol {

using namespace std::chrono_literals;

DataClient::DataClient(std::string endpoint, std::string instance)
    : endpoint_(std::move(endpoint)),
      instance_(std::move(instance))
{
    validate_id(instance_);
    service::Transport{endpoint_, {}, 0, {}}.validate();
}

data::proto::DataResponse
DataClient::call(data::proto::DataRequest request, std::chrono::milliseconds timeout) const
{
    request.set_version(1);
    request.set_service_id(instance_);
    request.set_correlation_id(next_correlation_id());
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto remaining = [&](std::chrono::milliseconds maximum) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left <= 0ms)
            throw Error(ErrorCode::unavailable, "data request timed out");
        return std::min(left, maximum);
    };
    auto channel = ipc::Channel::connect(endpoint_, remaining(5s));
    channel.send(request.SerializeAsString(), remaining(5s));
    const auto bytes = channel.receive(remaining(timeout));
    return decode_data_response(request, bytes);
}

data::proto::DataResponse
decode_data_response(const data::proto::DataRequest& request, const std::string& bytes)
{
    data::proto::DataResponse response;
    if (!response.ParseFromString(bytes))
        throw std::runtime_error("invalid data service response");
    validate_message(response);
    if (response.version() != 1 || response.service_id() != request.service_id()
        || response.correlation_id() != request.correlation_id())
        throw std::runtime_error("data response identity mismatch");
    log_rpc_result(
        "data-client",
        request,
        response,
        request.has_heartbeat(),
        {{"service_id", request.service_id()}});
    if (response.has_error())
        throw_remote_error(response.error().code(), response.error().message());
    using Request = data::proto::DataRequest;
    using Response = data::proto::DataResponse;
    const auto expected = [&] {
        switch (request.operation_case()) {
            case Request::kAcquisitionTask:
                return Response::kAcquisitionTask;
            case Request::kResources:
                return Response::kResources;
            case Request::kObservation:
                return Response::kObservation;
            case Request::kMaterialSources:
                return Response::kMaterialSources;
            case Request::kMaterialRead:
                return Response::kMaterialRead;
            case Request::kTableRetirement:
                return Response::kTableRetirement;
            case Request::kTableRetirements:
                return Response::kTableRetirements;
            case Request::kMinuteRetirements:
                return Response::kMinuteRetirements;
            case Request::kMinuteRetirement:
                return Response::kMinuteRetirement;
            case Request::kRetention:
                return Response::kRetention;
            case Request::kHistoryUsage:
                return Response::kHistoryUsage;
            case Request::kHeartbeat:
                return Response::kHealth;
            case Request::kQuiesce:
                return Response::kHealth;
            case Request::kSources:
                return Response::kSources;
            case Request::kVerifyConnection:
                return Response::kConnectionVerification;
            case Request::kDatasets:
                return Response::kDatasets;
            case Request::kRecord:
                return Response::kRecord;
            case Request::kMinutePage:
                return Response::kMinutePage;
            case Request::kHistoricalMargins:
                return Response::kHistoricalMargins;
            case Request::kTableRefreshPlan:
                return Response::kTableRefreshPlan;
            case Request::kMinuteRange:
                return Response::kMinuteRange;
            case Request::kMinuteEvidence:
                return Response::kMinuteEvidence;
            case Request::kBarDataset:
                return Response::kBarDataset;
            case Request::kCoverage:
                return Response::kCoverage;
            case Request::kSaveDataset:
                return Response::kSavedDataset;
            case Request::kSavedDataset:
                return Response::kSavedDataset;
            case Request::kSavedDatasets:
                return Response::kSavedDatasets;
            case Request::kUpdatePlan:
                return Response::kUpdatePlan;
            case Request::kDominantSeries:
                return Response::kDominantSeries;
            case Request::kAuthorizeDownload:
            case Request::kDownloadAuthorization:
                return Response::kDownloadAuthorization;
            case Request::kConfigureDownloadBudget:
                return Response::kDownloadBudget;
            case Request::kAcquireDownloadPermit:
                return Response::kDownloadPermit;
            case Request::kDownloadCredentials:
                return Response::kDownloadCredentials;
            case Request::kAllocateDownload:
                return Response::kDownloadDirectory;
            case Request::kPrepareDownload:
                return Response::kPreparedDownload;
            case Request::kPublishDownload:
                return Response::kPublishedDownload;
            case Request::kPublishedDownload:
                return Response::kPublishedDownload;
            case Request::kTableCoverage:
                return Response::kTableCoverage;
            case Request::kTablePage:
                return Response::kTablePage;
            case Request::kTablePlan:
                return Response::kTablePlan;
            case Request::kStoreTablePartition:
                return Response::kTablePartition;
            case Request::kTableSeries:
                return Response::kTableSeries;
            case Request::kTableProducts:
                return Response::kTableProducts;
            case Request::kTableDaily:
                return Response::kTableDaily;
            case Request::kTableContracts:
                return Response::kTableContracts;
            case Request::kTableListedContracts:
                return Response::kTableListedContracts;
            case Request::OPERATION_NOT_SET:
                return Response::RESULT_NOT_SET;
        }
        throw std::invalid_argument("unsupported data operation");
    }();
    if (expected == Response::RESULT_NOT_SET || response.result_case() != expected)
        throw Error(ErrorCode::unavailable, "unexpected data response");
    if (request.has_material_read()) {
        const auto& read = response.material_read();
        if ((request.material_read().has_expected_reference()
                && request.material_read().expected_reference().SerializeAsString()
                    != read.receipt().reference().SerializeAsString())
            || read.receipt().reference().id() != request.material_read().id()
            || read.receipt().reference().owner().service_id() != request.service_id()
            || read_material_text(read.receipt(), read.original()).SerializeAsString()
                != read.text().SerializeAsString())
            throw Error(
                ErrorCode::unavailable,
                "material response differs from its acquisition or original bytes");
    }
    return response;
}

} // namespace asterion::protocol
