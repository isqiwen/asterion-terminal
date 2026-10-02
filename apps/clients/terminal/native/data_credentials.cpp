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
#ifdef __APPLE__
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace asterion::terminal {
namespace fs = std::filesystem;
namespace {
void safe(const fs::path& path) {
  if (!path.is_absolute() || fs::is_symlink(path))
    throw std::invalid_argument("invalid data source credential storage");
}
#ifdef __APPLE__
struct Output {
  int status = 1;
  std::string text;
};
// Runs the helper with an optional stdin payload; no shell, bounded output.
Output run(const fs::path& helper, const std::vector<std::string>& arguments,
           const std::string& input) {
  int in[2], out[2];
  if (::pipe(in) != 0)
    throw std::runtime_error("credential store unavailable");
  if (::pipe(out) != 0) {
    ::close(in[0]);
    ::close(in[1]);
    throw std::runtime_error("credential store unavailable");
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, in[0], 0);
  posix_spawn_file_actions_adddup2(&actions, out[1], 1);
  for (const int fd : {in[0], in[1], out[0], out[1]})
    posix_spawn_file_actions_addclose(&actions, fd);
  std::vector<std::string> owned{helper.string()};
  owned.insert(owned.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& item : owned)
    argv.push_back(item.data());
  argv.push_back(nullptr);
  char* environment[] = {nullptr};
  pid_t pid = 0;
  const auto spawned =
      posix_spawn(&pid, helper.c_str(), &actions, nullptr, argv.data(), environment);
  posix_spawn_file_actions_destroy(&actions);
  ::close(in[0]);
  ::close(out[1]);
  Output result;
  if (spawned == 0) {
    for (std::size_t offset = 0; offset < input.size();) {
      const auto n = ::write(in[1], input.data() + offset, input.size() - offset);
      if (n <= 0)
        break;
      offset += static_cast<std::size_t>(n);
    }
  }
  ::close(in[1]);
  if (spawned == 0) {
    char buffer[1024];
    for (ssize_t n; (n = ::read(out[0], buffer, sizeof buffer)) > 0;)
      if (result.text.size() < 8192)
        result.text.append(buffer, static_cast<std::size_t>(n));
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  }
  ::close(out[0]);
  if (spawned != 0)
    throw std::runtime_error("credential store unavailable");
  return result;
}
class Keychain final : public CredentialStore {
public:
  explicit Keychain(fs::path helper) : helper_(std::move(helper)) {}
  std::optional<std::string> load(const std::string& account) override {
    const auto output = run(helper_, {"get", account}, {});
    if (output.status == 3)
      return std::nullopt;
    if (output.status != 0)
      throw Error(ErrorCode::unavailable, "keychain credential is unavailable");
    return output.text;
  }
  void store(const std::string& account, const std::string& secret) override {
    if (run(helper_, {"set", account}, secret).status != 0)
      throw Error(ErrorCode::unavailable, "cannot save credential to the keychain");
  }
  void erase(const std::string& account) override {
    if (run(helper_, {"delete", account}, {}).status != 0)
      throw Error(ErrorCode::unavailable, "cannot remove credential from the keychain");
  }

private:
  fs::path helper_;
};
#endif
} // namespace
std::shared_ptr<CredentialStore> keychain_store(const fs::path& helper) {
#ifdef __APPLE__
  if (helper.is_absolute() && fs::is_regular_file(helper) && !fs::is_symlink(helper))
    return std::make_shared<Keychain>(helper);
#else
  (void)helper;
#endif
  return nullptr;
}
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
  fs::create_directory(directory_);
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
