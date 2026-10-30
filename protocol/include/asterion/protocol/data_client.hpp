#pragma once
#include <string>
#include <asterion/data_service.pb.h>
#include <chrono>

namespace asterion::protocol {

// Shared identity and result validation for synchronous and event-loop transports.
data::proto::DataResponse decode_data_response(const data::proto::DataRequest&, const std::string&);

// A local connection target names a persistent data instance. Each call opens a
// bounded exchange; uncertain outcomes are returned to the owning coordinator.
// This client never retries a mutation or substitutes another data instance.
class DataClient
{
public:
    DataClient(std::string endpoint, std::string instance);
    data::proto::DataResponse call(
        data::proto::DataRequest,
        std::chrono::milliseconds timeout = std::chrono::seconds(120)) const;

    const std::string& instance() const noexcept
    {
        return instance_;
    }

private:
    std::string endpoint_, instance_;
};

} // namespace asterion::protocol
