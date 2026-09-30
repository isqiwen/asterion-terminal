#include "data_connections.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <fstream>
#include <regex>
#include <optional>
#include <algorithm>
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
          {"remember", connection.remember},
          {"credential", connection.remember ? connection.credential : ""}};
}
} // namespace
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
  if (document.size() != 9 || document.at("version") != 1 || document.at("id") != id)
    throw std::invalid_argument("invalid data connection storage");
  DataConnection connection{document.at("id"),       document.at("name"),
                            document.at("source"),   document.at("plugin_id"),
                            document.at("revision"), document.at("requests_per_minute"),
                            document.at("remember"), document.at("credential")};
  if (connection.name.empty() || connection.name.size() > 128 || connection.source.empty() ||
      connection.plugin_id.empty() || connection.revision.empty() ||
      connection.credential.size() > 256 ||
      !document.at("requests_per_minute").is_number_unsigned() ||
      connection.requests_per_minute < 1 || connection.requests_per_minute > 500)
    throw std::invalid_argument("invalid data connection storage");
  if (!connection.remember) {
    if (!connection.credential.empty())
      throw std::invalid_argument("invalid data connection storage");
    const auto found = session_credentials_.find(id);
    if (found != session_credentials_.end() && found->second.first == connection.revision)
      connection.credential = found->second.second;
  }
  return connection;
}
Json DataConnections::snapshot() const {
  safe(directory_);
  Json result = Json::array();
  if (!fs::exists(directory_))
    return result;
  for (const auto& entry : fs::directory_iterator(directory_)) {
    if (entry.path().extension() != ".json")
      continue;
    if (result.size() >= 128)
      throw std::invalid_argument("too many data connections");
    const auto connection = get(entry.path().stem().string());
    result.push_back({{"id", connection.id},
                      {"name", connection.name},
                      {"source", connection.source},
                      {"plugin_id", connection.plugin_id},
                      {"revision", connection.revision},
                      {"requests_per_minute", connection.requests_per_minute},
                      {"remember", connection.remember},
                      {"credential_ready", !connection.credential.empty()}});
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
      (connection.remember && !schema.remember_allowed()))
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
  if (!current && snapshot().size() >= 128)
    throw std::invalid_argument("too many data connections");
  connection.revision = unique_process_id();
  replace_file_durably(file, encode(connection).dump(), true);
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
  fs::remove(file);
  sync_directory(directory_);
  session_credentials_.erase(id);
}
} // namespace asterion::terminal
