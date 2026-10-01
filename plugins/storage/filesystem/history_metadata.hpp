#pragma once
#include <asterion/domain/history_identity.hpp>
#include <asterion/foundation/serialization.hpp>
namespace asterion::history_files {
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
