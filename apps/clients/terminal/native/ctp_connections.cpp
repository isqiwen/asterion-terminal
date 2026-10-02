#include "ctp_connections.hpp"
#include <algorithm>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <cctype>
#include <asterion/kernel/process/file_lock.hpp>
#include <charconv>
#include <fstream>
#include <regex>
namespace asterion::terminal {
namespace fs = std::filesystem;
namespace {
void safe(const fs::path& path) {
  if (!path.is_absolute() || fs::is_symlink(path))
    throw std::invalid_argument("invalid CTP connection storage");
}
// CTP identities: short printable text without spaces.
bool identity(const std::string& text, std::size_t limit, bool required) {
  return (required ? !text.empty() : true) && text.size() <= limit &&
         std::ranges::none_of(text, [](unsigned char c) { return c <= ' ' || c > '~'; });
}
bool front(const std::string& text) {
  const auto colon = text.rfind(':');
  if (!text.starts_with("tcp://") || text.size() > 64 || colon == std::string::npos || colon <= 6)
    return false;
  const auto host = text.substr(6, colon - 6), port = text.substr(colon + 1);
  int number = 0;
  const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
  return error == std::errc{} && end == port.data() + port.size() && number >= 1 &&
         number <= 65535 && std::ranges::all_of(host, [](unsigned char c) {
           return std::isalnum(c) || c == '.' || c == '-';
         });
}
void validate(const CtpConnection& c) {
  // A connection may be used for market data only: the application
  // identifier and either front may be left out until they are needed.
  if (c.name.empty() || c.name.size() > 128 || c.name.find('\0') != std::string::npos ||
      !identity(c.broker_id, 10, true) || !identity(c.user_id, 15, true) ||
      !identity(c.app_id, 32, false) || (!c.trade_front.empty() && !front(c.trade_front)) ||
      (!c.market_front.empty() && !front(c.market_front)) ||
      (c.trade_front.empty() && c.market_front.empty()))
    throw std::invalid_argument("invalid CTP connection settings");
}
Json encode(const CtpConnection& c) {
  return {{"version", 1},
          {"id", c.id},
          {"name", c.name},
          {"broker_id", c.broker_id},
          {"user_id", c.user_id},
          {"app_id", c.app_id},
          {"trade_front", c.trade_front},
          {"market_front", c.market_front},
          {"revision", c.revision}};
}
} // namespace
fs::path CtpConnections::path(const std::string& id) const {
  if (!std::regex_match(id, std::regex("[A-Za-z0-9][A-Za-z0-9_-]{0,63}")))
    throw std::invalid_argument("invalid CTP connection identity");
  safe(directory_);
  const auto file = directory_ / (id + ".json");
  safe(file);
  return file;
}
CtpConnection CtpConnections::get(const std::string& id) const {
  const auto file = path(id);
  if (!fs::is_regular_file(file) || fs::file_size(file) > 8192)
    throw Error(ErrorCode::not_found, "CTP connection is unavailable");
  std::ifstream input(file);
  const std::string contents{std::istreambuf_iterator<char>(input), {}};
  const auto document = parse_json(contents, 8192);
  if (document.size() != 9 || document.at("version") != 1 || document.at("id") != id)
    throw std::invalid_argument("invalid CTP connection storage");
  CtpConnection connection{document.at("id"),           document.at("name"),
                           document.at("broker_id"),    document.at("user_id"),
                           document.at("app_id"),       document.at("trade_front"),
                           document.at("market_front"), document.at("revision")};
  if (connection.revision.empty())
    throw std::invalid_argument("invalid CTP connection storage");
  validate(connection);
  return connection;
}
Json CtpConnections::snapshot() const {
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
Json CtpConnections::read_all() const {
  Json result = Json::array();
  for (const auto& entry : fs::directory_iterator(directory_)) {
    if (entry.path().extension() != ".json")
      continue;
    if (result.size() >= 64)
      break;
    const auto id = entry.path().stem().string();
    try {
      auto item = encode(get(id));
      item.erase("version");
      result.push_back(std::move(item));
    } catch (const std::exception&) {
      // Keep the file for inspection; the rest of the Terminal stays usable.
      result.push_back({{"id", id}, {"name", id}, {"error", "unreadable"}});
    }
  }
  std::sort(result.begin(), result.end(),
            [](const auto& left, const auto& right) { return left.at("name") < right.at("name"); });
  return result;
}
void CtpConnections::save(CtpConnection connection, const std::string& expected) {
  const auto file = path(connection.id);
  validate(connection);
  safe(directory_.parent_path());
  fs::create_directory(directory_);
  fs::permissions(directory_, fs::perms::owner_all);
  FileLock lock(directory_, "connections.lock");
  if ((fs::exists(file) ? get(connection.id).revision : std::string{}) != expected)
    throw Error(ErrorCode::conflict, "CTP connection changed; inspect again");
  if (!fs::exists(file) && read_all().size() >= 64)
    throw std::invalid_argument("too many CTP connections");
  connection.revision = unique_process_id();
  replace_file_durably(file, encode(connection).dump(), true);
  cached_.reset();
  // The first account with a market front supplies market data without a further step.
  if (!market() && !connection.market_front.empty())
    replace_file_durably(directory_ / "market", connection.id, true);
}
std::optional<CtpConnection> CtpConnections::market() const {
  safe(directory_);
  const auto file = directory_ / "market";
  safe(file);
  if (!fs::is_regular_file(file) || fs::file_size(file) > 64)
    return std::nullopt;
  std::ifstream input(file);
  std::string id;
  input >> id;
  try {
    return get(id);
  } catch (const std::exception&) {
    return std::nullopt; // Removed or unreadable: no market data account.
  }
}
void CtpConnections::select_market(const std::string& id) {
  if (get(id).market_front.empty())
    throw std::invalid_argument("CTP connection has no market front");
  FileLock lock(directory_, "connections.lock");
  replace_file_durably(directory_ / "market", id, true);
}
void CtpConnections::remove(const std::string& id, const std::string& expected) {
  const auto file = path(id);
  FileLock lock(directory_, "connections.lock");
  if (get(id).revision != expected)
    throw Error(ErrorCode::conflict, "CTP connection changed; inspect again");
  fs::remove(file);
  sync_directory(directory_);
  cached_.reset();
}
} // namespace asterion::terminal
