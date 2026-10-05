#pragma once
#include <asterion/domain/history_identity.hpp>
#include <asterion/foundation/serialization.hpp>
#include <chrono>
#include <charconv>
namespace asterion::history_files {
// A complete version becomes locally available when its manifest is published.
// This is never a provider publication time or a per-row observation time.
inline std::int64_t acquired_at(const Json& manifest) {
  const auto text = manifest.at("acquired_at_ns").get<std::string>();
  std::int64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || std::to_string(value) != text ||
      value < 0 || (value > 0) != manifest.at("complete").get<bool>() ||
      manifest.at("source_availability") != "unknown")
    throw std::invalid_argument("invalid historical availability evidence");
  return value;
}
inline void complete_version(Json& manifest, bool complete) {
  manifest["complete"] = complete;
  if (complete)
    manifest["acquired_at_ns"] =
        std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count());
}
inline Json encode_semantics(const HistorySemantics& s) {
  s.validate();
  return {{"source", s.source},           {"normalization", s.normalization},
          {"timezone", s.timezone},       {"timestamp_semantics", s.timestamp_semantics},
          {"amount_unit", s.amount_unit}, {"quantity_unit", s.quantity_unit}};
}
inline HistorySemantics decode_semantics(const Json& j) {
  HistorySemantics s{j.at("source"),      j.at("normalization"),
                     j.at("timezone"),    j.at("timestamp_semantics"),
                     j.at("amount_unit"), j.at("quantity_unit")};
  if (encode_semantics(s) != j)
    throw std::invalid_argument("unsupported historical data semantics");
  return s;
}
} // namespace asterion::history_files

namespace asterion::history_files {
inline bool valid_semantics(const Json& j) {
  (void)decode_semantics(j);
  return true;
}
} // namespace asterion::history_files
