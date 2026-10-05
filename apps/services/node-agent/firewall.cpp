#include "firewall.hpp"
#include <asterion/kernel/process/child.hpp>
#include <asio/ip/address.hpp>
#include <openssl/evp.h>
#include <fstream>
#include <regex>
#include <stdexcept>
namespace asterion::node {
void validate_firewall_source(const std::string& source) {
  asio::error_code error;
  auto ip = asio::ip::make_address(source, error);
  if (error || ip.is_unspecified() || ip.is_multicast() || source.find('%') != std::string::npos)
    throw std::invalid_argument("firewall source must be one explicit host IP");
}
std::string firewall_inspection(const std::string& os, const std::string& source) {
  if (!source.empty())
    validate_firewall_source(source);
  if (os != "linux" && os != "macos")
    throw std::invalid_argument("unsupported firewall platform");
  const auto assignment =
      source.empty() ? "source=${SSH_CONNECTION%% *}\n" : "source='" + source + "'\n";
  return "set -eu\n" + assignment +
         "case \"$source\" in ''|*[!0-9a-fA-F:.]*) exit 2;; esac\n"
         "backend=manual; state=manual; os=macos\n"
         "if test \"$(uname -s)\" = Linux; then\n"
         " os=linux\n"
         " if test -x /usr/sbin/ufw; then\n"
         "  backend=ufw; state=permission_required\n"
         "  if test \"$(id -u)\" = 0; then prefix=''; else prefix='sudo -n'; fi\n"
         "  if status=$(LC_ALL=C $prefix /usr/sbin/ufw status verbose 2>/dev/null); then\n"
         "   case \"$status\" in 'Status: active'*) state=active;; 'Status: inactive'*) "
         "state=disabled;; *) state=unknown;; esac\n"
         "  fi\n"
         " else state=unsupported; fi\n"
         "fi\n"
         "printf '{\"source\":\"%s\",\"backend\":\"%s\",\"state\":\"%s\",\"os\":\"%s\"}\\n' "
         "\"$source\" \"$backend\" \"$state\" \"$os\"\n";
}
std::string firewall_change(const std::string& os, const std::string& source, unsigned short port,
                            const std::string& rule, bool remove) {
  validate_firewall_source(source);
  if (!port || !std::regex_match(rule, std::regex("asterion-[a-f0-9]{32}")))
    throw std::invalid_argument("invalid owned firewall rule");
  const auto number = std::to_string(port);
  if (os != "linux")
    throw std::invalid_argument(
        "source-scoped firewall automation is unavailable on this platform");
  std::string script = "set -eu\nexport LC_ALL=C\nif test \"$(id -u)\" = 0; then prefix=''; else "
                       "prefix='sudo -n'; fi\nstatus=$($prefix /usr/sbin/ufw status verbose)\ncase "
                       "\"$status\" in 'Status: active'*) ;; *) exit 3;; esac\n";
  script += "owned=$(printf '%s\\n' \"$status\" | awk -v tag='# " + rule +
            "' 'index($0,tag) && substr($0,length($0)-length(tag)+1)==tag')\n";
  script += "if test -n \"$owned\"; then printf '%s\\n' \"$owned\" | awk -v port='" + number +
            "/tcp' -v source='" + source +
            "' '{line=$0;sub(/^[[:space:]]+/,\"\",line);sub(/ "
            "*\\(v6\\)/,\"\",line);split(line,f,/[[:space:]]+/);if(f[1]==port && f[2]==\"ALLOW\" "
            "&& f[3]==\"IN\" && f[4]==source)n++} END {exit n!=1 || NR!=1}';\n";
  const auto spec =
      "allow in proto tcp from '" + source + "' to any port " + number + " comment '" + rule + "'";
  if (remove)
    script += "$prefix /usr/sbin/ufw delete " + spec + " >/dev/null\n";
  script += "else\n";
  if (!remove) {
    script += "if printf '%s\\n' \"$status\" | awk -v port='" + number + "/tcp' -v source='" +
              source +
              "' '{line=$0;sub(/^[[:space:]]+/,\"\",line);sub(/ "
              "*\\(v6\\)/,\"\",line);split(line,f,/[[:space:]]+/);if(f[1]==port && "
              "f[4]==source)n++} END {exit n==0}'; then exit 4; fi\n";
    script += "$prefix /usr/sbin/ufw " + spec + " >/dev/null\n";
  } else
    script += ":\n";
  script += "fi\nstatus=$($prefix /usr/sbin/ufw status verbose)\n";
  script += "count=$(printf '%s\\n' \"$status\" | awk -v tag='# " + rule +
            "' 'substr($0,length($0)-length(tag)+1)==tag {n++} END {print n+0}')\n";
  script += "test \"$count\" = " + std::string(remove ? "0" : "1") + "\n";
  return script + "printf '{\"changed\":true}\\n'\n";
}
Json run_firewall_script(const std::string& script) {
  const auto file =
      std::filesystem::temp_directory_path() / ("asterion-firewall-" + unique_process_id());
  struct Cleanup {
    std::filesystem::path file;
    ~Cleanup() {
      std::error_code ec;
      std::filesystem::remove(file, ec);
    }
  } cleanup{file};
  ChildProcess child("/bin/sh", {"-c", script}, true, file);
  if (!child.wait(std::chrono::seconds(20)) || child.exit_code() != 0)
    throw std::runtime_error("firewall command failed; inspect permissions and existing rules");
  if (std::filesystem::file_size(file) > 65536)
    throw std::runtime_error("firewall report too large");
  std::ifstream input(file);
  return Json::parse(input);
}
} // namespace asterion::node
