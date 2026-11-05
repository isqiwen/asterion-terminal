#include <asterion/protocol/common.hpp>
#include <asterion/base/serialization.hpp>

namespace asterion::protocol {

::common::Decimal decimal(const Decimal& value)
{
    ::common::Decimal out;
    out.set_text(value.str());
    return out;
}

Decimal decimal(const ::common::Decimal& value)
{
    return Decimal::parse(value.text());
}

std::string iso_day(std::string_view day)
{
    if (day.empty())
        return {};
    if (day.size() != 8)
        throw std::invalid_argument("invalid CTP day: " + std::string(day));
    return std::string(day.substr(0, 4)) + "-" + std::string(day.substr(4, 2)) + "-"
        + std::string(day.substr(6, 2));
}

::common::ErrorCode error_code(ErrorCode code)
{
    return static_cast<::common::ErrorCode>(static_cast<int>(code) + 1);
}

::common::Error error(const std::exception& failure)
{
    ::common::Error out;
    out.set_code(error_code(classify(failure)));
    out.set_message(failure.what());
    return out;
}

void raise(const ::common::Error& error)
{
    const auto code = static_cast<int>(error.code()) - 1;
    throw Error(
        code < 0 || code >= static_cast<int>(error_codes.size()) ? ErrorCode::operation_failed
                                                                 : error_codes[code],
        error.message());
}

} // namespace asterion::protocol
