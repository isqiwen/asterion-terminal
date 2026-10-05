#pragma once
#include <memory>
#include <future>
#include <functional>
#include <exception>
#include <string>
#include <asterion/kernel/payload_budget.hpp>
#include <nlohmann/json.hpp>
namespace asterion::terminal {
// Native request boundary over one I/O state owner. Commands suspend for service
// or management work; other requests and background publications keep advancing.
// Each account and the node-management workflow have explicit admission gates.
// Callers retain Application until their requests finish.
class Application {
public:
  Application();
  ~Application();
  nlohmann::json dispatch(const nlohmann::json& request);
  // Encoded response or encoding failure, exactly once after successful admission.
  // Completion runs on the owner and must not throw or destroy Application.
  // The future completes after the callback returns; retain Application until then.
  using Completion = std::function<void(Payload, std::exception_ptr)>;
  std::future<void> request(std::string wire, Completion complete);

private:
  friend struct ApplicationTestAccess;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::terminal
