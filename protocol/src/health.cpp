#include <asterion/protocol/health.hpp>

namespace asterion::protocol {

runtime::proto::Progress encode_progress(Progress::Observation value)
{
    runtime::proto::Progress result;
    result.set_observed(value.observed);
    result.set_pending(value.pending);
    result.set_age_ms(value.age_ms);
    return result;
}

} // namespace asterion::protocol
