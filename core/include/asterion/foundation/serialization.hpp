#pragma once
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/foundation/time.hpp>
#include <nlohmann/json.hpp>
#include <charconv>
#include <set>
#include <string_view>
#include <vector>
namespace asterion {
using Json = nlohmann::json;
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
struct EventEnvelope {
  std::string id;
  std::string source;
  std::string type;
  Nanoseconds timestamp_ns;
  Json payload;
};
inline std::string encode_event(const EventEnvelope& event) {
  validate_id(event.id);
  validate_id(event.source);
  validate_id(event.type);
  if (!event.payload.is_object())
    throw Error(ErrorCode::invalid_request, "event payload must be an object");
  auto result = Json{{"version", 1},
                     {"id", event.id},
                     {"source", event.source},
                     {"type", event.type},
                     {"timestamp_ns", std::to_string(event.timestamp_ns)},
                     {"payload", event.payload}}
                    .dump();
  // Apply identical limits in both directions.
  static_cast<void>(parse_json(result));
  return result;
}
inline EventEnvelope decode_event(std::string_view wire) {
  const auto value = parse_json(wire);
  require_fields(value, {"version", "id", "source", "type", "timestamp_ns", "payload"});
  if (!value.at("version").is_number_integer() || value.at("version") != 1)
    throw Error(ErrorCode::invalid_request, "unsupported event version");
  EventEnvelope result{value.at("id").get<std::string>(), value.at("source").get<std::string>(),
                       value.at("type").get<std::string>(), 0, value.at("payload")};
  const auto timestamp = value.at("timestamp_ns").get<std::string>();
  const auto [end, error] =
      std::from_chars(timestamp.data(), timestamp.data() + timestamp.size(), result.timestamp_ns);
  if (error != std::errc{} || end != timestamp.data() + timestamp.size() ||
      std::to_string(result.timestamp_ns) != timestamp)
    throw Error(ErrorCode::invalid_request, "invalid canonical timestamp");
  static_cast<void>(encode_event(result));
  return result;
}
} // namespace asterion
