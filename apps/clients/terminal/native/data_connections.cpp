#include "data_connections.hpp"
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
    throw std::invalid_argument("invalid data connection storage");
}
Json encode(const DataConnection& connection) {
  return {{"version", 1},
          {"id", connection.id},
          {"name", connection.name},
          {"source", connection.source},
          {"plugin_id", connection.plugin_id},
          {"revision", connection.revision},
          {"requests_per_minute", connection.requests_per_minute},
          {"remember", connection.remember}};
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
std::string DataConnections::account(const std::string& id) const {
  // Isolated nodes (tests, development) never share keychain entries.
  return sha256_bytes(directory_.string()).substr(0, 16) + "/" + id;
}
fs::path DataConnections::path(const std::string& id) const {
  if (!std::regex_match(id, std::regex("[A-Za-z0-9][A-Za-z0-9_-]{0,63}")))
    throw std::invalid_argument("invalid data connection identity");
  safe(directory_);
  const auto file = directory_ / (id + ".json");
  safe(file);
  return file;
}
DataConnection DataConnections::get(const std::string& id) const {
  const auto file = path(id);
  if (!fs::is_regular_file(file) || fs::file_size(file) > 8192)
    throw Error(ErrorCode::not_found, "data connection is unavailable");
  std::ifstream input(file);
  const std::string contents{std::istreambuf_iterator<char>(input), {}};
  const auto document = parse_json(contents, 8192);
  if (document.size() != 8 || document.at("version") != 1 || document.at("id") != id)
    throw std::invalid_argument("invalid data connection storage");
  DataConnection connection{document.at("id"),       document.at("name"),
                            document.at("source"),   document.at("plugin_id"),
                            document.at("revision"), document.at("requests_per_minute"),
                            document.at("remember"), ""};
  if (connection.name.empty() || connection.name.size() > 128 || connection.source.empty() ||
      connection.plugin_id.empty() || connection.revision.empty() ||
      !document.at("requests_per_minute").is_number_unsigned() ||
      connection.requests_per_minute < 1 || connection.requests_per_minute > 500)
    throw std::invalid_argument("invalid data connection storage");
  if (connection.remember) {
    if (remembered_)
      connection.credential = remembered_->load(account(id)).value_or("");
  } else {
    const auto found = session_credentials_.find(id);
    if (found != session_credentials_.end() && found->second.first == connection.revision)
      connection.credential = found->second.second;
  }
  return connection;
}
Json DataConnections::snapshot() const {
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
Json DataConnections::read_all() const {
  Json result = Json::array();
  for (const auto& entry : fs::directory_iterator(directory_)) {
    if (entry.path().extension() != ".json")
      continue;
    if (result.size() >= 128)
      break;
    const auto id = entry.path().stem().string();
    try {
      const auto connection = get(id);
      result.push_back({{"id", connection.id},
                        {"name", connection.name},
                        {"source", connection.source},
                        {"plugin_id", connection.plugin_id},
                        {"revision", connection.revision},
                        {"requests_per_minute", connection.requests_per_minute},
                        {"remember", connection.remember},
                        {"credential_ready", !connection.credential.empty()}});
    } catch (const std::exception&) {
      // Keep the file for inspection; the rest of the Terminal stays usable.
      result.push_back({{"id", id}, {"name", id}, {"error", "unreadable"}});
    }
  }
  std::sort(result.begin(), result.end(),
            [](const auto& left, const auto& right) { return left.at("name") < right.at("name"); });
  return result;
}
void DataConnections::save(DataConnection connection, const std::string& expected,
                           const std::string& action,
                           const data::v1::HistoryConnectionSchema& schema) {
  const auto file = path(connection.id);
  safe(directory_.parent_path());
  fs::create_directory(directory_);
  fs::permissions(directory_, fs::perms::owner_all);
  FileLock lock(directory_, "connections.lock");
  std::optional<DataConnection> current;
  if (fs::exists(file))
    current = get(connection.id);
  if ((current ? current->revision : "") != expected)
    throw Error(ErrorCode::conflict, "data connection changed; inspect again");
  if (connection.name.empty() || connection.name.size() > 128 || connection.source.empty() ||
      connection.plugin_id.empty() || !connection.requests_per_minute ||
      connection.requests_per_minute > schema.requests_per_minute_max() ||
      (connection.remember && (!schema.remember_allowed() || !remembered_)))
    throw std::invalid_argument("invalid data connection settings");
  if (current &&
      (current->source != connection.source || current->plugin_id != connection.plugin_id))
    throw std::invalid_argument("data connection source cannot change");
  if (action == "keep") {
    if (!current)
      throw std::invalid_argument("invalid data connection credential action");
    connection.credential = current->credential;
  } else if (action == "clear")
    connection.credential.clear();
  else if (action != "replace")
    throw std::invalid_argument("invalid data connection credential action");
  if (connection.credential.find('\0') != std::string::npos ||
      connection.credential.size() > schema.credential_max_length() ||
      (schema.credential_required() && connection.credential.empty()))
    throw std::invalid_argument("invalid native plugin credential");
  if (!current && read_all().size() >= 128)
    throw std::invalid_argument("too many data connections");
  connection.revision = unique_process_id();
  // The secret changes first: a failed file write leaves the old revision.
  if (remembered_) {
    if (connection.remember && !connection.credential.empty())
      remembered_->store(account(connection.id), connection.credential);
    else
      remembered_->erase(account(connection.id));
  }
  replace_file_durably(file, encode(connection).dump(), true);
  cached_.reset();
  if (connection.remember)
    session_credentials_.erase(connection.id);
  else
    session_credentials_[connection.id] = {connection.revision, connection.credential};
}
void DataConnections::remove(const std::string& id, const std::string& expected) {
  const auto file = path(id);
  FileLock lock(directory_, "connections.lock");
  if (get(id).revision != expected)
    throw Error(ErrorCode::conflict, "data connection changed; inspect again");
  if (remembered_)
    remembered_->erase(account(id));
  fs::remove(file);
  sync_directory(directory_);
  cached_.reset();
  session_credentials_.erase(id);
}
} // namespace asterion::terminal
