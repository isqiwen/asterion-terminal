#pragma once
#include <asterion/base/decimal.hpp>
#include <asterion/base/error.hpp>
#include <asterion/common.pb.h>
#include <string>
#include <string_view>

namespace asterion::protocol {

::common::Decimal decimal(const Decimal& value);
Decimal decimal(const ::common::Decimal& value);
// CTP writes days as YYYYMMDD; the protocol as YYYY-MM-DD. Empty stays empty.
std::string iso_day(std::string_view day);
::common::Error error(const std::exception& failure);
::common::ErrorCode error_code(ErrorCode code);
// Raises a peer's error as this process's Error.
[[noreturn]] void raise(const ::common::Error& error);

} // namespace asterion::protocol
