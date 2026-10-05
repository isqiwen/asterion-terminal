#pragma once
#include <cstddef>
#include <memory>
#include <string>
namespace asterion {
namespace ipc {
class RpcClient;
}
using Payload = std::shared_ptr<const std::string>;
// Copies share one byte allowance. A payload keeps its charge until its last
// reader releases it, including readers on another thread or beyond a connection.
// Counts serialized bytes, not allocator capacity or decoded object memory.
class PayloadBudget {
public:
  explicit PayloadBudget(std::size_t limit);
  Payload retain(std::string bytes) const;
  std::size_t limit() const;

private:
  friend class ipc::RpcClient;
  std::shared_ptr<std::string> allocate(std::size_t bytes) const;
  struct State;
  std::shared_ptr<State> state_;
};
} // namespace asterion
