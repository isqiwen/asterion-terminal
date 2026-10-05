#include <asterion/protocol/data_client.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/rpc_host.hpp>
#include <asterion/kernel/trace.hpp>
namespace asterion::protocol {
using namespace std::chrono_literals;
DataClient::DataClient(std::string endpoint, std::string instance)
    : endpoint_(std::move(endpoint)), instance_(std::move(instance)) {
  validate_id(instance_);
  service::Transport{endpoint_, {}, 0, {}}.validate();
}
DataClient::DataClient(std::string host, unsigned short port, ipc::TlsIdentity tls,
                       std::string instance)
    : host_(std::move(host)), instance_(std::move(instance)), port_(port), tls_(std::move(tls)) {
  validate_id(instance_);
  service::Transport{{}, host_, port_, tls_}.validate();
}
data::v1::DataResponse DataClient::call(data::v1::DataRequest request,
                                        std::chrono::milliseconds timeout) const {
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
  auto exchange = [&](auto channel) {
    channel.send(request.SerializeAsString(), remaining(5s));
    return channel.receive(remaining(timeout));
  };
  const auto bytes = host_.empty()
                         ? exchange(ipc::Channel::connect(endpoint_, remaining(5s)))
                         : exchange(ipc::TlsChannel::connect(host_, port_, tls_, remaining(5s)));
  return decode_data_response(request, bytes);
}
data::v1::DataResponse decode_data_response(const data::v1::DataRequest& request,
                                            const std::string& bytes) {
  data::v1::DataResponse response;
  if (!response.ParseFromString(bytes))
    throw std::runtime_error("invalid data service response");
  validate_message(response);
  if (response.version() != 1 || response.service_id() != request.service_id() ||
      response.correlation_id() != request.correlation_id())
    throw std::runtime_error("data response identity mismatch");
  log_rpc_result("data-client", request, response, request.has_heartbeat(),
                 {{"service_id", request.service_id()}});
  if (response.has_error())
    throw_remote_error(response.error().code(), response.error().message());
  using Request = data::v1::DataRequest;
  using Response = data::v1::DataResponse;
  const auto expected = [&] {
    switch (request.operation_case()) {
    case Request::kHistoryUsage:
      return Response::kHistoryUsage;
    case Request::kHeartbeat:
      return Response::kHealth;
    case Request::kQuiesce:
      return Response::kHealth;
    case Request::kSources:
      return Response::kSources;
    case Request::kCatalog:
      return Response::kCatalog;
    case Request::kVerifyConnection:
      return Response::kConnectionVerification;
    case Request::kDatasets:
      return Response::kDatasets;
    case Request::kRecord:
      return Response::kRecord;
    case Request::kMinutePage:
      return Response::kMinutePage;
    case Request::kDailyPage:
      return Response::kDailyPage;
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
    case Request::kDailyFactorDataset:
      return Response::kDailyFactorDataset;
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
    case Request::OPERATION_NOT_SET:
      return Response::RESULT_NOT_SET;
    }
    throw std::invalid_argument("unsupported data operation");
  }();
  if (expected == Response::RESULT_NOT_SET || response.result_case() != expected)
    throw Error(ErrorCode::unavailable, "unexpected data response");
  return response;
}
} // namespace asterion::protocol
