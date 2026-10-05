#pragma once
#include "service_endpoint.hpp"
#include "service_io.hpp"
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/task.hpp>
#include <memory>
#include <chrono>
namespace asterion::terminal {
class Application;
class TaskClient {
public:
  [[nodiscard]] static std::future<std::shared_ptr<TaskClient>> open(ServiceIo&, ServiceEndpoint);
  ~TaskClient();
  TaskClient(const TaskClient&) = delete;
  TaskClient& operator=(const TaskClient&) = delete;
  [[nodiscard]] std::future<Json> status() const;
  [[nodiscard]] std::future<void> page(unsigned before_sequence);
  ServiceEndpoint endpoint() const;
  [[nodiscard]] std::future<void> submit(const std::string&, const backtest::v1::BacktestRequest&);
  [[nodiscard]] std::future<void> submit(const std::string&, const factor::v1::FactorRequest&);
  [[nodiscard]] std::future<void> submit(const std::string&, const factor::v1::DailyFactorRequest&);
  [[nodiscard]] std::future<void> submit_download(const std::string& id,
                                                  const std::string& authorization);
  [[nodiscard]] std::future<Json> history_usage(const std::string&);
  [[nodiscard]] static std::future<Json>
  inspect_history_usage(ServiceIo&, const ServiceEndpoint&, const std::string&,
                        std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] std::future<void> action(const std::string& id, const std::string& action);
  [[nodiscard]] std::future<Json> result(const std::string& id);

private:
  friend class Application;
  struct Read {
    std::shared_ptr<const Json> page;
    Json connection;
    Json render() const;
    bool operator==(const Read&) const = default;
  };
  // Application collects selected service views in one I/O owner turn.
  Read owner_view() const;
  TaskClient(ServiceIo&, ServiceEndpoint);
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
} // namespace asterion::terminal
