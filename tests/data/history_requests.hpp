#pragma once
#include "history_minutes.hpp"
#include <asterion/foundation/serialization.hpp>
#include <string>

// Download definitions for tests, written as the fields a client fills in and
// validated the way the history store validates a definition it is given.
namespace asterion::testing_support {

inline data::proto::MinuteDownload minute_request(const Json& value)
{
    require_fields(
        value,
        {"contract_id",
            "source",
            "source_instrument",
            "interval_minutes",
            "begin_ns",
            "end_ns",
            "requests_per_minute"});
    data::proto::MinuteDownload input;
    input.set_contract_id(value.at("contract_id").get<std::string>());
    input.set_source(value.at("source").get<std::string>());
    input.set_source_instrument(value.at("source_instrument").get<std::string>());
    input.set_interval_minutes(value.at("interval_minutes").get<unsigned>());
    input.set_begin_ns(std::stoll(value.at("begin_ns").get<std::string>()));
    input.set_end_ns(std::stoll(value.at("end_ns").get<std::string>()));
    input.set_requests_per_minute(value.at("requests_per_minute").get<unsigned>());
    static_cast<void>(history_files::minute_range(input));
    return input;
}

} // namespace asterion::testing_support
