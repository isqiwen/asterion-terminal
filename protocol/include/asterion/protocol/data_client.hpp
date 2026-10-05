#pragma once
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <asterion/v1/data_service.pb.h>
#include <chrono>
namespace asterion::protocol {
// Shared identity and result validation for synchronous and event-loop transports.
data::v1::DataResponse decode_data_response(const data::v1::DataRequest&, const std::string&);
// A connection target names a persistent data instance. Each call opens a
// bounded exchange; uncertain outcomes are returned to the owning coordinator.
// This client never retries a mutation or substitutes another data instance.
class DataClient {
public:
  DataClient(std::string endpoint, std::string instance);
  DataClient(std::string host, unsigned short port, ipc::TlsIdentity tls, std::string instance);
  data::v1::DataResponse call(data::v1::DataRequest,
                              std::chrono::milliseconds timeout = std::chrono::seconds(120)) const;
  const std::string& instance() const noexcept { return instance_; }

private:
  std::string endpoint_, host_, instance_;
  unsigned short port_ = 0;
  ipc::TlsIdentity tls_;
};
} // namespace asterion::protocol
