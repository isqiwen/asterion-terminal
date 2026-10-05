#pragma once
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/foundation/time.hpp>
#include <nlohmann/json.hpp>
#include <charconv>
#include <new>
#include <stdexcept>
#include <set>
#include <string_view>
#include <vector>
namespace asterion {
using Json = nlohmann::json;
// Wire code for an exception crossing a process or language boundary.
inline ErrorCode classify(const std::exception& error) noexcept {
  if (const auto* typed = dynamic_cast<const Error*>(&error))
    return typed->code();
  if (dynamic_cast<const Json::exception*>(&error) ||
      dynamic_cast<const std::invalid_argument*>(&error) ||
      dynamic_cast<const std::domain_error*>(&error) ||
      dynamic_cast<const std::out_of_range*>(&error) ||
      dynamic_cast<const std::length_error*>(&error) ||
      dynamic_cast<const std::overflow_error*>(&error) ||
      dynamic_cast<const std::underflow_error*>(&error) ||
      dynamic_cast<const std::range_error*>(&error))
    return ErrorCode::invalid_request;
  if (dynamic_cast<const std::logic_error*>(&error))
    return ErrorCode::conflict;
  if (dynamic_cast<const std::bad_alloc*>(&error))
    return ErrorCode::resource_exhausted;
  if (dynamic_cast<const std::runtime_error*>(&error))
    return ErrorCode::operation_failed;
  return ErrorCode::internal_error;
}
// Reject ambiguous duplicate keys as well as excessively large/deep input.
inline Json parse_json(std::string_view source, std::size_t max_bytes = 65536) {
  if (source.size() > max_bytes)
    throw Error(ErrorCode::resource_exhausted, "JSON size limit exceeded");
  std::vector<std::set<std::string>> keys;
  return Json::parse(source, [&](int depth, Json::parse_event_t event, Json& value) {
    if (depth > 64)
      throw Error(ErrorCode::invalid_request, "JSON nesting limit exceeded");
    if (event == Json::parse_event_t::object_start)
      keys.emplace_back();
    if (event == Json::parse_event_t::object_end)
      keys.pop_back();
    if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
      throw Error(ErrorCode::invalid_request, "duplicate JSON field");
    return true;
  });
}
inline void require_fields(const Json& value, std::initializer_list<std::string_view> fields) {
  if (!value.is_object() || value.size() != fields.size())
    throw Error(ErrorCode::invalid_request, "unexpected object fields");
  for (auto field : fields)
    if (!value.contains(field))
      throw Error(ErrorCode::invalid_request, "missing object field");
}
} // namespace asterion
