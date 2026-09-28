#pragma once
#include <asterion/domain/account.hpp>
#include <asterion/kernel/plugin.hpp>
namespace asterion {
// Normalized execution contract shared by application and trusted execution plugins.
class ExecutionPort : public Plugin {
public:
  virtual void submit(LimitOrder order, Offset offset) = 0;
  virtual void cancel(const std::string& order_id) = 0;
  virtual Json snapshot() const = 0;
};
} // namespace asterion
