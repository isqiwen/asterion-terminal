#include "data_credentials.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <fstream>
#include <regex>
#include <optional>
#include <algorithm>
#include <asterion/kernel/process/artifact.hpp>
namespace asterion::terminal {
namespace fs = std::filesystem;
namespace {
void safe(const fs::path& path) {
  if (!path.is_absolute() || fs::is_symlink(path))
    throw std::invalid_argument("invalid data source credential storage");
}
} // namespace
std::string DataCredentials::account(const std::string& provider) const {
  // Isolated nodes (tests, development) never share keychain entries. The
  // keychain helper takes a restricted character set, so the provider is
  // named by its digest.
  return sha256_bytes(directory_.string()).substr(0, 16) + "/" +
         sha256_bytes(provider).substr(0, 32);
}
fs::path DataCredentials::path(const std::string& provider) const {
  if (!std::regex_match(provider, std::regex("[A-Za-z0-9][A-Za-z0-9._-]{0,127}")))
    throw std::invalid_argument("invalid data source identity");
  safe(directory_);
  const auto file = directory_ / (provider + ".json");
  safe(file);
  return file;
}
std::optional<DataCredential> DataCredentials::find(const std::string& provider) const {
  const auto file = path(provider);
  if (!fs::exists(file))
    return std::nullopt;
  if (!fs::is_regular_file(file) || fs::file_size(file) > 8192)
    throw std::invalid_argument("invalid data source credential storage");
  std::ifstream input(file);
  const std::string contents{std::istreambuf_iterator<char>(input), {}};
  const auto document = parse_json(contents, 8192);
  if (document.size() != 4 || document.at("version") != 1 || document.at("provider") != provider ||
      !document.at("requests_per_minute").is_number_unsigned() ||
      !document.at("remember").is_boolean())
    throw std::invalid_argument("invalid data source credential storage");
  DataCredential saved{provider, document.at("requests_per_minute"), document.at("remember"), ""};
  if (saved.requests_per_minute < 1 || saved.requests_per_minute > 500)
    throw std::invalid_argument("invalid data source credential storage");
  if (saved.remember) {
    if (remembered_)
      saved.credential = remembered_->load(account(provider)).value_or("");
  } else if (const auto found = session_.find(provider); found != session_.end())
    saved.credential = found->second;
  return saved;
}
Json DataCredentials::snapshot() const {
  safe(directory_);
  std::error_code missing;
  const auto time = fs::last_write_time(directory_, missing);
  if (missing)
    return Json::array();
  // Saves and removals replace files by rename, which updates the directory time.
  if (!cached_ || time != cached_time_) {
    cached_ = read_all();
    cached_time_ = time;
  }
  return *cached_;
}
Json DataCredentials::read_all() const {
  Json result = Json::array();
  for (const auto& entry : fs::directory_iterator(directory_)) {
    if (entry.path().extension() != ".json")
      continue;
    if (result.size() >= 128)
      break;
    const auto provider = entry.path().stem().string();
    try {
      const auto saved = find(provider).value();
      result.push_back({{"provider", saved.provider},
                        {"requests_per_minute", saved.requests_per_minute},
                        {"remember", saved.remember},
                        {"credential_ready", !saved.credential.empty()}});
    } catch (const std::exception&) {
      // Keep the file for inspection; the rest of the Terminal stays usable.
      result.push_back({{"provider", provider}, {"error", "unreadable"}});
    }
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.at("provider") < right.at("provider");
  });
  return result;
}
void DataCredentials::save(DataCredential value, const DataCredentialLimits& limits) {
  const auto file = path(value.provider);
  safe(directory_.parent_path());
  create_directories_durably(directory_);
  fs::permissions(directory_, fs::perms::owner_all);
  FileLock lock(directory_, "credentials.lock");
  if (!value.requests_per_minute || value.requests_per_minute > limits.requests_per_minute_max ||
      (value.remember && (!limits.remember_allowed || !remembered_)))
    throw std::invalid_argument("invalid data source credential settings");
  // An empty credential keeps the one already held.
  if (value.credential.empty())
    if (const auto current = find(value.provider))
      value.credential = current->credential;
  if (value.credential.find('\0') != std::string::npos ||
      value.credential.size() > limits.credential_max_length)
    throw std::invalid_argument("invalid data source credential settings");
  if (limits.credential_required && value.credential.empty())
    throw std::invalid_argument("data source credential is required");
  // The secret changes first: a failed file write leaves the old settings.
  if (remembered_) {
    if (value.remember && !value.credential.empty())
      remembered_->store(account(value.provider), value.credential);
    else
      remembered_->erase(account(value.provider));
  }
  replace_file_durably(file,
                       Json{{"version", 1},
                            {"provider", value.provider},
                            {"requests_per_minute", value.requests_per_minute},
                            {"remember", value.remember}}
                           .dump(),
                       true);
  cached_.reset();
  if (value.remember)
    session_.erase(value.provider);
  else
    session_[value.provider] = value.credential;
}
void DataCredentials::clear(const std::string& provider) {
  const auto file = path(provider);
  if (!fs::exists(file))
    return;
  FileLock lock(directory_, "credentials.lock");
  if (remembered_)
    remembered_->erase(account(provider));
  fs::remove(file);
  sync_directory(directory_);
  cached_.reset();
  session_.erase(provider);
}
} // namespace asterion::terminal
