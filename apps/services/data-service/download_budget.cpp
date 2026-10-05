#include "download_budget.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <fstream>
namespace asterion::data {
namespace {
constexpr Nanoseconds minute = 60'000'000'000LL;
std::string account_key(const std::string& provider, const std::string& credential) {
  validate_id(provider);
  if (credential.size() > 256)
    throw std::invalid_argument("invalid native plugin credential");
  return sha256_bytes(provider + "\n" + credential);
}
} // namespace
DownloadBudget::DownloadBudget(std::filesystem::path directory, std::shared_ptr<const Clock> clock)
    : root_(std::move(directory)), clock_(std::move(clock)) {
  create_directories_durably(root_);
  const auto recovered_after = clock_->monotonic_now() + minute;
  for (const auto& file : std::filesystem::directory_iterator(root_)) {
    const auto& path = file.path();
    if (path.extension() != ".pb")
      continue;
    const auto key = path.stem().string();
    if (key.size() != 64 || key.find_first_not_of("0123456789abcdef") != std::string::npos ||
        file.is_symlink() || !file.is_regular_file() || file.file_size() > 4096)
      throw std::invalid_argument("invalid download budget policy");
    std::ifstream stream(path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
    v1::DownloadBudgetPolicy policy;
    if (!stream || !policy.ParseFromString(bytes))
      throw std::invalid_argument("invalid download budget policy");
    protocol::validate_message(policy);
    validate_id(policy.provider_id());
    if (policy.version() != 1 || policy.requests_per_minute() < 1 ||
        policy.requests_per_minute() > 500)
      throw std::invalid_argument("invalid download budget policy");
    // Monotonic admissions cannot survive a restart. One full quiet window
    // prevents a fresh burst without journaling every provider request.
    windows_.emplace(
        key, Window{policy.provider_id(), policy.requests_per_minute(), recovered_after, {}});
  }
}
void DownloadBudget::require_available() const {
  if (failed_)
    throw Error(ErrorCode::unavailable, "download budget persistence failed; restart data service");
}
DownloadBudget::Configuration DownloadBudget::configure(const std::string& provider,
                                                        const std::string& credential,
                                                        unsigned limit) const {
  require_available();
  if (limit < 1 || limit > 500)
    throw std::invalid_argument("invalid download request budget");
  Configuration result{account_key(provider, credential), {}};
  if (const auto found = windows_.find(result.account);
      found != windows_.end() && found->second.provider != provider)
    throw std::invalid_argument("invalid download budget policy");
  result.policy.set_version(1);
  result.policy.set_provider_id(provider);
  result.policy.set_requests_per_minute(limit);
  return result;
}
void DownloadBudget::persist(const Configuration& configuration) const {
  replace_file_durably(root_ / (configuration.account + ".pb"),
                       configuration.policy.SerializeAsString());
}
void DownloadBudget::committed(const Configuration& configuration) {
  require_available();
  auto& current = windows_[configuration.account];
  current.provider = configuration.policy.provider_id();
  current.limit = configuration.policy.requests_per_minute();
  // Reconfiguration preserves admissions and any restart quiet period.
}
v1::DownloadPermit DownloadBudget::acquire(const std::string& provider,
                                           const std::string& credential) {
  require_available();
  const auto found = windows_.find(account_key(provider, credential));
  if (found == windows_.end())
    throw std::invalid_argument("download request budget is not configured");
  auto& current = found->second;
  if (current.provider != provider)
    throw std::invalid_argument("invalid download budget policy");
  const auto now = clock_->monotonic_now();
  while (!current.granted.empty() && current.granted.front() + minute <= now)
    current.granted.pop_front();
  auto ready = current.not_before;
  if (current.granted.size() >= current.limit)
    ready = std::max(ready, current.granted[current.granted.size() - current.limit] + minute);
  v1::DownloadPermit result;
  if (now < ready) {
    result.set_retry_after_ms(static_cast<unsigned>((ready - now + 999999) / 1000000));
    return result;
  }
  current.granted.push_back(now);
  result.set_granted(true);
  return result;
}
} // namespace asterion::data
