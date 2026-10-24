#pragma once
#include "service_endpoint.hpp"
#include "service_io.hpp"
#include <asterion/intelligence.pb.h>

namespace asterion::terminal {

PolledTask<intelligence::proto::IntelligenceResponse>
intelligence_call(ServiceIo&, const ServiceEndpoint&, intelligence::proto::IntelligenceRequest);

} // namespace asterion::terminal
