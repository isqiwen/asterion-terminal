#pragma once
#include <asterion/kernel/plugin.hpp>
#include <asterion/foundation/serialization.hpp>
namespace asterion {
// Ordered, durable commits; host serializes calls. Failure may have committed:
// implementation must refuse further writes until reopened and recovered.
class JournalPort : public Plugin {
public:
  static constexpr std::size_t page_size = 64;
  // Up to page_size consecutive records, starting at the given sequence.
  virtual std::vector<Json> read(std::uint64_t first) const = 0;
  virtual void append(const Json& record) = 0;
};
} // namespace asterion
