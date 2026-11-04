#pragma once
#include <asterion/system/progress.hpp>
#include <asterion/trading.pb.h>

namespace asterion::protocol {

runtime::proto::Progress encode_progress(Progress::Observation value);

} // namespace asterion::protocol
