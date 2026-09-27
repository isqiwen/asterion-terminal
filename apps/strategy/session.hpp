#pragma once
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/strategy.pb.h>
#include <filesystem>
#include <memory>
namespace asterion::strategy {
// One writer and one immutable stream per directory. Calls are serialized by
// the host. No account, broker, order submission or strategy sandbox is owned
// here.
class Session {
public:
  Session(const std::filesystem::path &directory, const std::string &session_id,
          const v1::Config *create = nullptr);
  ~Session();
  v1::Receipt apply(const v1::Event &event);
  v1::Snapshot snapshot() const;
  const v1::Config &config() const noexcept;
  std::uint64_t processed() const noexcept;
  void verify_config(const v1::Config &config) const;
  bool recovery_required() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::strategy
