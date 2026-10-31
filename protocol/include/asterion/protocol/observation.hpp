#pragma once
#include <asterion/market.pb.h>
#include <asterion/data_service.pb.h>
#include <asterion/base/serialization.hpp>

namespace asterion::protocol {

inline constexpr std::size_t max_observation_bytes = 4 * 1024 * 1024;
void validate_observation_reference(const resource::proto::Object&);
void validate_observation(const market::proto::ObservationCapture&);
Json decode_observation(const market::proto::ObservationCapture&);
Json decode_observation_status(const data::proto::ObservationStatus&);

} // namespace asterion::protocol
