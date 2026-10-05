#pragma once
#include <asterion/foundation/time.hpp>
#include <asterion/v1/data_service.pb.h>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
namespace asterion::data {
// The Data I/O owner owns all quota windows. Recovery and persist() run on the
// writer; persist only reads the immutable directory and supplied configuration.
class DownloadBudget {
public:
  struct Configuration {
    std::string account;
    v1::DownloadBudgetPolicy policy;
  };
  DownloadBudget(std::filesystem::path directory,
                 std::shared_ptr<const Clock> clock = std::make_shared<SystemClock>());
  Configuration configure(const std::string& provider, const std::string& credential,
                          unsigned limit) const;
  void persist(const Configuration&) const;
  void committed(const Configuration&);
  void persistence_failed() noexcept { failed_ = true; }
  v1::DownloadPermit acquire(const std::string& provider, const std::string& credential);

private:
  struct Window {
    std::string provider;
    unsigned limit = 0;
    Nanoseconds not_before = 0;
    std::deque<Nanoseconds> granted;
  };
  const std::filesystem::path root_;
  std::shared_ptr<const Clock> clock_;
  std::map<std::string, Window> windows_;
  bool failed_ = false;
  void require_available() const;
};
} // namespace asterion::data
