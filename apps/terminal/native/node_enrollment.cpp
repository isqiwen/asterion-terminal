#include "node_enrollment.hpp"
#include "firewall.hpp"
#include "remote_bundle.hpp"
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <openssl/evp.h>
#include <fstream>
#include <sstream>
#include <regex>
#include <cstdlib>
#include <cstring>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif
namespace asterion::terminal {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace {
std::string utf8(const fs::path& p) {
  const auto s = p.generic_u8string();
  return {s.begin(), s.end()};
}
fs::path path(const std::string& s) {
  return fs::path(std::u8string(s.begin(), s.end()));
}
void valid_id(const std::string& s) {
  if (s == "local" || !std::regex_match(s, std::regex("[A-Za-z0-9][A-Za-z0-9_-]{0,40}")))
    throw std::invalid_argument("invalid remote node name");
}
std::string quote(const std::string& s) {
  std::string r = "'";
  for (char c : s) {
    if (!c || c == '\n' || c == '\r')
      throw std::invalid_argument("invalid shell argument");
    r += c == '\'' ? "'\\''" : std::string(1, c);
  }
  return r + "'";
}
std::string batch_quote(const std::string& s) {
  if (s.find_first_of("\r\n\"*?[]") != std::string::npos)
    throw std::invalid_argument("unsupported upload path characters");
  std::string r = "\"";
  for (char c : s) {
    if (c == '\\')
      r += '\\';
    r += c;
  }
  return r + "\"";
}
fs::path state_root() {
  const char* test = std::getenv("ASTERION_NODE_DIRECTORY");
  if (test)
    return path(test) / "enrollments";
#ifdef _WIN32
  const char* home = std::getenv("LOCALAPPDATA");
#else
  const char* home = std::getenv("HOME");
#endif
  if (!home)
    throw std::runtime_error("local user data directory unavailable");
  return path(home) / ".asterion" / "nodes";
}
void write(const fs::path& p, const std::string& content) {
  if (fs::is_symlink(p))
    throw std::invalid_argument("enrollment path cannot be a symlink");
  std::ofstream out(p, std::ios::binary);
  out << content;
  out.close();
  if (!out)
    throw std::runtime_error("cannot write enrollment state");
}
fs::path tool(const char* name) {
  if (const char* test = std::getenv("ASTERION_SSH_TOOL_DIRECTORY")) {
    const auto root = path(test);
    if (!root.is_absolute())
      throw std::invalid_argument("SSH tool directory must be absolute");
    return root / name;
  }
#ifdef _WIN32
  wchar_t directory[MAX_PATH];
  auto n = GetSystemDirectoryW(directory, MAX_PATH);
  if (!n || n >= MAX_PATH)
    throw std::runtime_error("OpenSSH is unavailable");
  return fs::path(directory) / "OpenSSH" / (std::string(name) + ".exe");
#else
  return fs::path("/usr/bin") / name;
#endif
}
void private_directory(const fs::path& directory) {
#ifdef _WIN32
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    throw std::runtime_error("cannot identify local user");
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> buffer(size);
  const bool ok = GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != FALSE;
  CloseHandle(token);
  LPWSTR sid = nullptr;
  if (!ok || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
    throw std::runtime_error("cannot identify local user");
  const std::wstring acl = L"D:P(A;OICI;FA;;;" + std::wstring(sid) + L")";
  LocalFree(sid);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1,
                                                            &descriptor, nullptr))
    throw std::runtime_error("cannot protect local SSH directory");
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
  const bool made = CreateDirectoryW(directory.c_str(), &attributes) != FALSE;
  LocalFree(descriptor);
  if (!made)
    throw std::runtime_error("cannot create private SSH directory");
#else
  if (::mkdir(directory.c_str(), 0700) != 0)
    throw std::runtime_error("cannot create private SSH directory");
#endif
}
void validate_key_path(const fs::path& file, bool directory) {
  if (fs::is_symlink(file) || (directory ? !fs::is_directory(file) : !fs::is_regular_file(file)))
    throw std::invalid_argument("invalid managed SSH key path");
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(file.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    throw std::invalid_argument("managed SSH keys cannot use reparse points");
#else
  struct stat info{};
  if (::lstat(file.c_str(), &info) != 0 || info.st_uid != ::geteuid() || (info.st_mode & 0077) ||
      (!directory && info.st_nlink != 1))
    throw std::invalid_argument("managed SSH key permissions must be private to this user");
#endif
}
fs::path key_directory(const std::string& id) {
  valid_id(id);
  const auto base = state_root() / ".ssh-keys";
  validate_key_path(base, true);
  const auto directory = base / id;
  validate_key_path(directory, true);
  return directory;
}
std::string read_key_file(const fs::path& file) {
  validate_key_path(file, false);
  if (fs::file_size(file) > 65536)
    throw std::invalid_argument("managed SSH key is too large");
  std::ifstream input(file, std::ios::binary);
  std::string value{std::istreambuf_iterator<char>(input), {}};
  if (!input.eof() && input.fail())
    throw std::runtime_error("cannot read managed SSH key");
  return value;
}
std::string selected_ssh_key(const Json& p) {
  const auto source = p.at("key_source").get<std::string>();
  const auto supplied = p.at("private_key").get<std::string>();
  if (source == "provided")
    return supplied;
  if (source != "managed" || !supplied.empty())
    throw std::invalid_argument("select exactly one SSH key source");
  return read_key_file(key_directory(p.at("id")) / "identity");
}

std::string powershell(const std::string& script) {
  std::string wide;
  for (unsigned char c : script) {
    if (c > 127)
      throw std::invalid_argument("installer script must be ASCII");
    wide += static_cast<char>(c);
    wide += '\0';
  }
  std::string encoded(4 * ((wide.size() + 2) / 3), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                  reinterpret_cast<const unsigned char*>(wide.data()),
                  static_cast<int>(wide.size()));
  return "powershell.exe -NoProfile -NonInteractive -EncodedCommand " + encoded;
}
// OpenSSH consumes a local identity file. Never persist it in a saved profile or upload it.
class SshIdentity {
  fs::path file_;
#ifdef _WIN32
  HANDLE handle_{INVALID_HANDLE_VALUE};
#else
  int handle_{-1};
#endif
public:
  explicit SshIdentity(const std::string& key) {
    if (key.size() > 65536 || key.find('\0') != std::string::npos ||
        !key.starts_with("-----BEGIN ") || key.find("PRIVATE KEY-----") == std::string::npos ||
        key.find("-----END ") == std::string::npos)
      throw std::invalid_argument("paste the complete SSH private key");
#ifdef _WIN32
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
      throw std::runtime_error("cannot identify SSH user");
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    const bool read = GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != FALSE;
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!read ||
        !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
      throw std::runtime_error("cannot identify SSH user");
    const std::wstring acl = L"D:P(A;;FA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr))
      throw std::runtime_error("cannot protect SSH identity");
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    file_ = fs::temp_directory_path() / ("asterion-ssh-" + unique_process_id());
    handle_ = CreateFileW(file_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                          &attributes, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    LocalFree(descriptor);
    if (handle_ == INVALID_HANDLE_VALUE)
      throw std::runtime_error("cannot create SSH identity");
    const auto content = key + "\n";
    DWORD written = 0;
    if (!WriteFile(handle_, content.data(), static_cast<DWORD>(content.size()), &written,
                   nullptr) ||
        written != content.size() || !FlushFileBuffers(handle_)) {
      CloseHandle(handle_);
      DeleteFileW(file_.c_str());
      throw std::runtime_error("cannot prepare SSH identity");
    }
    CloseHandle(handle_);
#else
    auto name = (fs::temp_directory_path() / "asterion-ssh-XXXXXX").string();
    handle_ = mkstemp(name.data());
    if (handle_ < 0)
      throw std::runtime_error("cannot create SSH identity");
    file_ = name;
    const auto content = key + "\n";
    std::size_t offset = 0;
    while (offset < content.size()) {
      const auto count = ::write(handle_, content.data() + offset, content.size() - offset);
      if (count <= 0) {
        ::close(handle_);
        ::unlink(file_.c_str());
        throw std::runtime_error("cannot prepare SSH identity");
      }
      offset += static_cast<std::size_t>(count);
    }
    ::close(handle_);
#endif
  }
  SshIdentity(const SshIdentity&) = delete;
  SshIdentity& operator=(const SshIdentity&) = delete;
  ~SshIdentity() {
#ifdef _WIN32
    DeleteFileW(file_.c_str());
#else
    ::unlink(file_.c_str());
#endif
  }
  const fs::path& file() const { return file_; }
};
std::string quote_script(const std::string& script) {
  std::string result = "'";
  for (char c : script) {
    if (!c)
      throw std::invalid_argument("invalid script");
    result += c == '\'' ? "'\\''" : std::string(1, c);
  }
  return result + "'";
}
struct Ssh {
  std::vector<std::string> options;
  std::string host, user, port;
  explicit Ssh(const Json& p, const fs::path& key)
      : host(p.at("host")), user(p.at("username")), port(p.at("ssh_port")) {
    if (!std::regex_match(host, std::regex("[A-Za-z0-9][A-Za-z0-9.:-]{0,252}")) ||
        !std::regex_match(user, std::regex("[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}")))
      throw std::invalid_argument("invalid SSH host or user");
    const auto known = path(p.at("known_hosts").get<std::string>());
    if (!known.is_absolute() || !fs::is_regular_file(known) ||
        utf8(known).find_first_of("%\r\n\"") != std::string::npos)
      throw std::invalid_argument("choose a known_hosts file verified through a trusted channel");
    options = {"-F", "none",
               "-o", "BatchMode=yes",
               "-o", "StrictHostKeyChecking=yes",
               "-o", "UpdateHostKeys=no",
               "-o", "GlobalKnownHostsFile=none",
               "-o", "UserKnownHostsFile=\"" + utf8(known) + "\"",
               "-o", "ConnectTimeout=10",
               "-o", "ConnectionAttempts=1",
               "-o", "ServerAliveInterval=5",
               "-o", "ServerAliveCountMax=2",
               "-o", "ForwardAgent=no",
               "-o", "ClearAllForwardings=yes",
               "-o", "PasswordAuthentication=no",
               "-o", "KbdInteractiveAuthentication=no"};
    options.insert(options.end(),
                   {"-o", "PreferredAuthentications=publickey", "-o", "IdentitiesOnly=yes", "-o",
                    "IdentityAgent=none", "-i", utf8(key)});
  }
  bool command(const std::string& cmd, std::chrono::milliseconds timeout = 20s) {
    auto args = options;
    args.insert(args.end(), {"-p", port, "-l", user, "--", host, cmd});
    ChildProcess child(tool("ssh"), args, true);
    return child.wait(timeout) && child.exit_code() == 0;
  }
  Json report(const std::string& script, const std::string& os) {
    const auto output = fs::temp_directory_path() / ("asterion-ssh-report-" + unique_process_id());
    struct Cleanup {
      fs::path file;
      ~Cleanup() {
        std::error_code error;
        fs::remove(file, error);
      }
    } cleanup{output};
    const auto cmd = os == "windows" ? powershell(script) : "sh -c " + quote_script(script);
    auto args = options;
    args.insert(args.end(), {"-p", port, "-l", user, "--", host, cmd});
    ChildProcess child(tool("ssh"), args, true, output);
    if (!child.wait(30s) || child.exit_code() != 0)
      throw std::runtime_error("SSH inspection failed; verify host identity, login permission and "
                               "target Linux platform");
    if (fs::file_size(output) > 65536)
      throw std::runtime_error("SSH report too large");
    std::ifstream input(output);
    return Json::parse(input);
  }
  void upload(const fs::path& batch) {
    auto args = options;
    args.insert(args.end(),
                {"-P", port, "-b", utf8(batch),
                 user + "@" + (host.find(':') == std::string::npos ? host : "[" + host + "]")});
    ChildProcess child(tool("sftp"), args, true);
    if (!child.wait(120s) || child.exit_code() != 0)
      throw std::runtime_error(
          "SSH upload failed; check authentication, host identity and target directory permission");
  }
};
unsigned short port(const Json& p, const char* key) {
  const auto value = p.at(key).get<std::string>();
  if (!std::regex_match(value, std::regex("[0-9]{1,5}")))
    throw std::invalid_argument("invalid port");
  const auto n = std::stoul(value);
  if (!n || n > 65535)
    throw std::invalid_argument("invalid port");
  return static_cast<unsigned short>(n);
}
NodeEndpoint endpoint(const fs::path& root, const Json& j) {
  return {j.at("id"),
          j.at("host"),
          j.at("port").get<unsigned short>(),
          {utf8(root / "ca.crt"), utf8(root / "client.crt"), utf8(root / "client.key")}};
}
} // namespace
namespace {
fs::path firewall_record(const Json& p) {
  const auto id = p.at("id").get<std::string>();
  valid_id(id);
  return state_root() / ".firewall" /
         (id + "-" + std::to_string(port(p, "firewall_port")) + ".json");
}
Json owned_firewall(const Json& p) {
  const auto file = firewall_record(p);
  if (!fs::exists(file))
    return nullptr;
  if (fs::is_symlink(file.parent_path()) || fs::is_symlink(file) || fs::file_size(file) > 65536)
    throw std::invalid_argument("invalid firewall ownership record");
  std::ifstream input(file);
  auto record = Json::parse(input);
  for (const char* field : {"host", "username", "ssh_port"})
    if (record.at(field) != p.at(field))
      throw std::invalid_argument("firewall rule belongs to a different machine configuration");
  return record;
}
Json probe_firewall(const Json& p, Ssh& ssh, const std::string& os) {
  (void)p;
  auto report = ssh.report(node::firewall_inspection(os), os);
  node::validate_firewall_source(report.at("source").get<std::string>());
  if (report.size() != 4 || report.at("os") != os || !report.at("backend").is_string() ||
      !report.at("state").is_string())
    throw std::invalid_argument("invalid firewall inspection response");
  return report;
}
} // namespace
Json prepare_ssh_key(const std::string& id) {
  valid_id(id);
  const auto root = state_root();
  if (fs::is_symlink(root))
    throw std::invalid_argument("invalid enrollment root");
  fs::create_directories(root);
  FileLock lock(root, "ssh-keys.lock");
  const auto base = root / ".ssh-keys";
  if (!fs::exists(base) && !fs::is_symlink(base))
    private_directory(base);
  validate_key_path(base, true);
  const auto directory = base / id;
  if (!fs::exists(directory) && !fs::is_symlink(directory)) {
    if (fs::exists(root / id / "enrollment.json"))
      throw std::invalid_argument("existing node has no local SSH key; an administrator must "
                                  "re-authorize explicitly, no replacement key is generated");
    private_directory(directory);
    try {
      ChildProcess generate(tool("ssh-keygen"),
                            {"-q", "-t", "ed25519", "-N", "", "-C", "asterion-terminal", "-f",
                             utf8(directory / "identity")});
      if (!generate.wait(30s) || generate.exit_code() != 0)
        throw std::runtime_error("cannot generate local SSH identity; check OpenSSH installation");
#ifndef _WIN32
      fs::permissions(directory / "identity.pub", fs::perms::owner_read | fs::perms::owner_write);
#endif
    } catch (...) {
      std::error_code ignored;
      fs::remove_all(directory, ignored);
      throw;
    }
  }
  const auto checked = key_directory(id);
  validate_key_path(checked / "identity", false);
  const auto public_key = read_key_file(checked / "identity.pub");
  if (!public_key.starts_with("ssh-ed25519 ") || public_key.find("PRIVATE") != std::string::npos)
    throw std::invalid_argument("invalid managed public key");
  const auto report = checked / ("public-check-" + unique_process_id());
  struct Cleanup {
    fs::path file;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove(file, ignored);
    }
  } cleanup{report};
  ChildProcess derive(tool("ssh-keygen"), {"-y", "-P", "", "-f", utf8(checked / "identity")}, false,
                      report);
  if (!derive.wait(10s) || derive.exit_code() != 0)
    throw std::runtime_error("cannot validate managed SSH identity");
  const auto actual = read_key_file(report);
  std::string type, body, actual_type, actual_body;
  std::istringstream(public_key) >> type >> body;
  std::istringstream(actual) >> actual_type >> actual_body;
  if (type != actual_type || body != actual_body)
    throw std::invalid_argument("managed public/private key mismatch; original files preserved");
  return {{"id", id}, {"public_key", public_key}};
}
Json inspect_node_firewall(const Json& p) {
  (void)port(p, "ssh_port");
  const auto target_port = port(p, "firewall_port");
  valid_id(p.at("id"));
  const auto action = p.at("firewall_action").get<std::string>();
  if (action != "allow" && action != "remove")
    throw std::invalid_argument("unsupported firewall action");
  const HostPlatform platform{"linux", ""};
  SshIdentity identity(selected_ssh_key(p));
  Ssh ssh(p, identity.file());
  auto observed = probe_firewall(p, ssh, "linux");
  const auto owned = owned_firewall(p);
  if (ssh.user == "asterion" && observed.at("state") == "active")
    observed["state"] = "read_only";
  const auto source = action == "remove" && !owned.is_null()
                          ? owned.at("source").get<std::string>()
                          : observed.at("source").get<std::string>();
  if (action == "allow" && !owned.is_null() && owned.at("source") != source)
    throw std::invalid_argument(
        "source address changed; remove the previous owned rule before allowing a new source");
  return {
      {"id", p.at("id")},
      {"host", p.at("host")},
      {"os", platform.os},
      {"port", target_port},
      {"source", source},
      {"observed_source", observed.at("source")},
      {"backend", observed.at("backend")},
      {"state", observed.at("state")},
      {"action", action},
      {"rule",
       owned.is_null() ? "asterion-" + unique_process_id() : owned.at("rule").get<std::string>()},
      {"can_apply", observed.at("state") == "active" &&
                        (observed.at("backend") == "ufw" || observed.at("backend") == "windows") &&
                        (action == "allow" || !owned.is_null())},
      {"owned", !owned.is_null()},
      {"verification", "not_checked"}};
}
Json change_node_firewall(const Json& p, const Json& plan) {
  SshIdentity identity(selected_ssh_key(p));
  Ssh ssh(p, identity.file());
  const auto os = plan.at("os").get<std::string>();
  const auto now = probe_firewall(p, ssh, os);
  if (!plan.at("can_apply").get<bool>() || now.at("source") != plan.at("observed_source") ||
      now.at("backend") != plan.at("backend") || now.at("state") != "active")
    throw std::invalid_argument("firewall inspection changed; inspect and confirm again");
  const auto file = firewall_record(p);
  const bool remove = plan.at("action") == "remove";
  if (fs::is_symlink(state_root()) || fs::is_symlink(file.parent_path()))
    throw std::invalid_argument("invalid firewall state directory");
  fs::create_directories(file.parent_path());
#ifndef _WIN32
  fs::permissions(file.parent_path(), fs::perms::owner_all);
#endif
  FileLock ownership(file.parent_path(), "firewall.lock");
  const auto current = owned_firewall(p);
  if (!current.is_null() &&
      (current.at("rule") != plan.at("rule") || current.at("source") != plan.at("source")))
    throw std::invalid_argument("owned firewall rule changed; inspect again");
  if (remove) {
    const auto owned = owned_firewall(p);
    if (owned.is_null() || owned.at("rule") != plan.at("rule") ||
        owned.at("source") != plan.at("source"))
      throw std::invalid_argument("no matching owned firewall rule");
  } else {
    if (fs::is_symlink(state_root()) || fs::is_symlink(file.parent_path()))
      throw std::invalid_argument("invalid firewall state directory");
    fs::create_directories(file.parent_path());
#ifndef _WIN32
    fs::permissions(file.parent_path(), fs::perms::owner_all);
#endif
    Json record = plan;
    for (const char* field : {"host", "username", "ssh_port"})
      record[field] = p.at(field);
    // Persist ownership intent first: failed or lost replies can be inspected/retried without
    // losing the rule identity.
    write(file, record.dump());
  }
  const auto result =
      ssh.report(node::firewall_change(os, plan.at("source"), plan.at("port").get<unsigned short>(),
                                       plan.at("rule"), remove),
                 os);
  if (result != Json{{"changed", true}})
    throw std::runtime_error("unexpected firewall change response");
  if (remove)
    fs::remove(file);
  Json completed = plan;
  completed["can_apply"] = false;
  completed["state"] = remove ? "removed" : "applied";
  completed["verification"] = "not_checked";
  if (!remove) {
    try {
      const auto config = enrolled_node(p.at("id"));
      if (config.host != p.at("host").get<std::string>())
        throw std::invalid_argument("installed node identity differs");
      try {
        auto channel = ipc::TlsChannel::connect(config.host, plan.at("port").get<unsigned short>(),
                                                config.tls, 3s);
        completed["verification"] = "tls_reachable";
      } catch (const std::exception&) {
        completed["verification"] = "unreachable";
      }
    } catch (const std::exception&) {
      completed["verification"] = "pending_install";
    }
  }
  return completed;
}
NodeEndpoint enrolled_node(const std::string& id) {
  valid_id(id);
  const auto root = state_root() / id;
  const auto file = root / "enrollment.json";
  if (fs::is_symlink(root) || fs::is_symlink(file) || !fs::is_regular_file(file) ||
      fs::file_size(file) > 65536)
    throw std::invalid_argument("node SSH identity is not configured");
  std::ifstream in(file);
  Json j = Json::parse(in);
  if (j.at("version") != 1 || j.at("id") != id || j.at("os") != "linux")
    throw std::invalid_argument("invalid enrollment");
  return endpoint(root, j);
}
NodeEndpoint enroll_node(const Json& p) {
  const auto id = p.at("id").get<std::string>();
  valid_id(id);
  const auto agent_port = port(p, "agent_port");
  (void)port(p, "ssh_port");
  SshIdentity identity(selected_ssh_key(p));
  Ssh ssh(p, identity.file());
  if (ssh.user != "asterion" || agent_port < 1024)
    throw std::invalid_argument("Linux nodes use the asterion account created by the initializer; "
                                "the Agent port must be at least 1024");
  const auto detected = ssh.report(
      "set -eu\ntest \"$(uname -s)\" = Linux\ncase \"$(uname -m)\" in x86_64) arch=x86_64;; *) "
      "exit 3;; esac\nprintf '{\"os\":\"linux\",\"arch\":\"%s\"}\\n' \"$arch\"\n",
      "linux");
  if (detected.size() != 2 || detected.at("os") != "linux")
    throw std::invalid_argument("remote deployment supports Linux only");
  const HostPlatform platform{"linux", detected.at("arch").get<std::string>()};
  const auto binary = bundled_linux_program(platform.arch, "asterion-node-agent");
  const auto digest = sha256_file(binary);
  const auto probe =
      "test \"$(uname -s)\" = Linux && test \"$(uname -m)\" = " + quote("x86_64") +
      " && test \"$(id -un)\" = asterion && sudo -n /usr/local/sbin/asterion-host --manage check";
  if (!ssh.command(probe))
    throw std::runtime_error("SSH verification failed; check host identity, authentication, Linux "
                             "platform and host initialization");
  const auto base = state_root();
  if (fs::is_symlink(base))
    throw std::invalid_argument("invalid enrollment root");
  fs::create_directories(base);
#ifndef _WIN32
  fs::permissions(base, fs::perms::owner_all);
#endif
  FileLock lock(base, "enrollment.lock");
  const auto root = base / id;
  Json config{{"version", 1},         {"id", id},           {"host", ssh.host},
              {"port", agent_port},   {"artifact", digest}, {"os", platform.os},
              {"arch", platform.arch}};
  if (fs::exists(root)) {
    if (fs::is_symlink(root))
      throw std::invalid_argument("invalid enrollment path");
    std::ifstream in(root / "enrollment.json");
    Json previous = Json::parse(in);
    if (previous != config)
      throw std::invalid_argument(
          "node name is used by another installation; its identity and services are not replaced");
    try {
      NodeClient existing(endpoint(root, config));
      return endpoint(root, config);
    } catch (const std::exception&) {
    }
  } else {
    fs::create_directory(root);
#ifndef _WIN32
    fs::permissions(root, fs::perms::owner_all);
#endif
    create_node_identity(root, ssh.host);
    write(root / "enrollment.json", config.dump());
  }
  const auto stage = ".asterion-install-" + unique_process_id();
  if (!ssh.command("umask 077; mkdir " + quote(stage)))
    throw std::runtime_error("cannot create private SSH installation staging directory");
  const auto destination = "/var/lib/asterion/nodes/" + id;
  const auto bind = ssh.host.find(':') == std::string::npos ? "0.0.0.0" : "::";
  write(root / "install.json", config.dump());
  std::string script =
      "#!/bin/sh\nset -eu\numask 077\nstage=" + quote(stage) + "\nroot=" + quote(destination) +
      "\nif test -e \"$root\"; then cmp \"$stage/install.json\" \"$root/install.json\"; fi\n";
  script += "actual=$(" + std::string("sha256sum") +
            " \"$stage/agent\" | cut -d ' ' -f 1)\ntest \"$actual\" = " + quote(digest) +
            "\nmkdir -p \"$root/state\"\nchmod 700 \"$root\"\ncp \"$stage/install.json\" "
            "\"$root/install.json\"\nif test -e \"$root/asterion-node-agent\"; then cmp "
            "\"$stage/agent\" \"$root/asterion-node-agent\"; else cp \"$stage/agent\" "
            "\"$root/asterion-node-agent\"; fi\nchmod 700 \"$root/asterion-node-agent\"\n";
  for (const auto* name : {"ca.crt", "server.crt", "server.key"})
    script += "if test -e \"$root/" + std::string(name) + "\"; then cmp \"$stage/" + name +
              "\" \"$root/" + name + "\"; else cp \"$stage/" + name + "\" \"$root/" + name +
              "\"; fi\nchmod 600 \"$root/" + name + "\"\n";
  script += "sudo -n /usr/local/sbin/asterion-host --manage install " + quote(id) + " " +
            std::to_string(agent_port) + " " + quote(bind) + "\n";
  write(root / "install", script);
  std::string batch;
  for (const auto* file : {"ca.crt", "server.crt", "server.key", "install.json", "install"})
    batch += "put " + batch_quote(utf8(root / file)) + " " + batch_quote(stage + "/" + file) + "\n";
  batch += "put " + batch_quote(utf8(binary)) + " " + batch_quote(stage + "/agent") + "\n";
  write(root / "upload.batch", batch);
  try {
    ssh.upload(root / "upload.batch");
    const auto execute = "sh " + quote(stage + "/install");
    if (!ssh.command(execute, 60s))
      throw std::runtime_error(
          "system service installation incomplete; local identity kept and existing remote "
          "installation untouched, check the target service and permissions");
  } catch (...) {
    ssh.command("rm -rf -- " + quote(stage));
    throw;
  }
  ssh.command("rm -rf -- " + quote(stage));
  const auto config_endpoint = endpoint(root, config);
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  for (;;) {
    try {
      NodeClient node(config_endpoint);
      const auto status = node.status();
      if (status.at("health").at("os") != platform.os ||
          status.at("health").at("arch") != platform.arch)
        throw std::invalid_argument("installed Agent platform mismatch");
      return config_endpoint;
    } catch (const Error&) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Agent installed but its TCP/mTLS heartbeat is unreachable; check "
                                 "the management port firewall, then connect without reinstalling");
      std::this_thread::sleep_for(500ms);
    }
  }
}
} // namespace asterion::terminal
