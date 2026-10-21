#include "node_enrollment.hpp"
#include "node_paths.hpp"
#include "agent_client.hpp"
#include "firewall.hpp"
#include "remote_bundle.hpp"
#include "remote_maintenance_script.hpp"
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <openssl/evp.h>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <stdexcept>
#include <unistd.h>
#include <sys/stat.h>

namespace asterion::terminal {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::string utf8(const fs::path& p)
{
    const auto s = p.generic_u8string();
    return {s.begin(), s.end()};
}

fs::path path(const std::string& s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}

void valid_id(const std::string& s)
{
    if (s == "local" || !std::regex_match(s, std::regex("[A-Za-z0-9][A-Za-z0-9_-]{0,40}")))
        throw std::invalid_argument("invalid remote node name");
}

std::string quote(const std::string& s)
{
    std::string r = "'";
    for (char c : s) {
        if (!c || c == '\n' || c == '\r')
            throw std::invalid_argument("invalid shell argument");
        r += c == '\'' ? "'\\''" : std::string(1, c);
    }
    return r + "'";
}

std::string batch_quote(const std::string& s)
{
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

void write(const fs::path& p, const std::string& content)
{
    if (fs::is_symlink(p))
        throw std::invalid_argument("enrollment path cannot be a symlink");
    replace_file_durably(p, content);
}

fs::path tool(const char* name)
{
    if (const auto test = environment_path("ASTERION_SSH_TOOL_DIRECTORY")) {
        const auto root = *test;
        if (!root.is_absolute())
            throw std::invalid_argument("SSH tool directory must be absolute");
        return root / name;
    }
    return fs::path("/usr/bin") / name;
}

void private_directory(const fs::path& directory)
{
    if (::mkdir(directory.c_str(), 0700) != 0)
        throw std::runtime_error("cannot create private SSH directory");
}

void validate_key_path(const fs::path& file, bool directory)
{
    if (fs::is_symlink(file) || (directory ? !fs::is_directory(file) : !fs::is_regular_file(file)))
        throw std::invalid_argument("invalid managed SSH key path");
    struct stat info{};
    if (::lstat(file.c_str(), &info) != 0 || info.st_uid != ::geteuid() || (info.st_mode & 0077)
        || (!directory && info.st_nlink != 1))
        throw std::invalid_argument("managed SSH key permissions must be private to this user");
}

fs::path key_directory(const std::string& id)
{
    valid_id(id);
    const auto base = node_enrollment_directory() / ".ssh-keys";
    validate_key_path(base, true);
    const auto directory = base / id;
    validate_key_path(directory, true);
    return directory;
}

std::string read_key_file(const fs::path& file)
{
    validate_key_path(file, false);
    if (fs::file_size(file) > 65536)
        throw std::invalid_argument("managed SSH key is too large");
    std::ifstream input(file, std::ios::binary);
    std::string value{std::istreambuf_iterator<char>(input), {}};
    if (!input.eof() && input.fail())
        throw std::runtime_error("cannot read managed SSH key");
    return value;
}

std::string selected_ssh_key(const Json& p)
{
    const auto source = p.at("key_source").get<std::string>();
    const auto supplied = p.at("private_key").get<std::string>();
    if (source == "provided")
        return supplied;
    if (source != "managed" || !supplied.empty())
        throw std::invalid_argument("select exactly one SSH key source");
    return read_key_file(key_directory(p.at("id")) / "identity");
}

// OpenSSH consumes a local identity file. Never persist it in a saved profile or upload it.
class SshIdentity
{
    fs::path file_;
    int handle_{-1};

public:
    explicit SshIdentity(const std::string& key)
    {
        if (key.size() > 65536 || key.find('\0') != std::string::npos
            || !key.starts_with("-----BEGIN ") || key.find("PRIVATE KEY-----") == std::string::npos
            || key.find("-----END ") == std::string::npos)
            throw std::invalid_argument("paste the complete SSH private key");
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
    }

    SshIdentity(const SshIdentity&) = delete;
    SshIdentity& operator=(const SshIdentity&) = delete;

    ~SshIdentity()
    {
        ::unlink(file_.c_str());
    }

    const fs::path& file() const
    {
        return file_;
    }
};

std::string quote_script(const std::string& script)
{
    std::string result = "'";
    for (char c : script) {
        if (!c)
            throw std::invalid_argument("invalid script");
        result += c == '\'' ? "'\\''" : std::string(1, c);
    }
    return result + "'";
}

struct Ssh
{
    std::vector<std::string> options;
    std::string host, user, port, last_error;

    explicit Ssh(const Json& p, const fs::path& key)
        : host(p.at("host")),
          user(p.at("username")),
          port(p.at("ssh_port"))
    {
        if (!std::regex_match(host, std::regex("[A-Za-z0-9][A-Za-z0-9.:-]{0,252}"))
            || !std::regex_match(user, std::regex("[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}")))
            throw std::invalid_argument("invalid SSH host or user");
        const auto known = path(p.at("known_hosts").get<std::string>());
        if (!known.is_absolute() || !fs::is_regular_file(known)
            || utf8(known).find_first_of("%\r\n\"") != std::string::npos)
            throw std::invalid_argument(
                "choose a known_hosts file verified through a trusted channel");
        options = {
            "-F",
            "none",
            "-o",
            "BatchMode=yes",
            "-o",
            "StrictHostKeyChecking=yes",
            "-o",
            "UpdateHostKeys=no",
            "-o",
            "GlobalKnownHostsFile=none",
            "-o",
            "UserKnownHostsFile=\"" + utf8(known) + "\"",
            "-o",
            "ConnectTimeout=10",
            "-o",
            "ConnectionAttempts=1",
            "-o",
            "ServerAliveInterval=5",
            "-o",
            "ServerAliveCountMax=2",
            "-o",
            "ForwardAgent=no",
            "-o",
            "ClearAllForwardings=yes",
            "-o",
            "PasswordAuthentication=no",
            "-o",
            "KbdInteractiveAuthentication=no"
        };
        options.insert(
            options.end(),
            {"-o",
                "PreferredAuthentications=publickey",
                "-o",
                "IdentitiesOnly=yes",
                "-o",
                "IdentityAgent=none",
                "-i",
                utf8(key)});
    }

    bool command(const std::string& cmd, std::chrono::milliseconds timeout = 20s)
    {
        auto args = options;
        args.insert(args.end(), {"-p", port, "-l", user, "--", host, cmd});
        const auto output =
            fs::temp_directory_path() / ("asterion-ssh-output-" + unique_process_id());

        struct Cleanup
        {
            fs::path file;

            ~Cleanup()
            {
                std::error_code ignored;
                fs::remove(file, ignored);
            }
        } cleanup{output};

        ChildProcess child(tool("ssh"), args, true, output, true);
        const bool completed = child.wait(timeout);
        const bool success = completed && child.exit_code() == 0;
        last_error.clear();
        if (!success) {
            std::ifstream input(output);
            const auto size = fs::file_size(output);
            if (size > 4096)
                input.seekg(static_cast<std::streamoff>(size - 4096));
            last_error.assign(std::istreambuf_iterator<char>(input), {});
            if (!completed)
                last_error += "SSH operation timed out";
        }
        return success;
    }

    Json report(const std::string& script)
    {
        const auto output =
            fs::temp_directory_path() / ("asterion-ssh-report-" + unique_process_id());

        struct Cleanup
        {
            fs::path file;

            ~Cleanup()
            {
                std::error_code error;
                fs::remove(file, error);
            }
        } cleanup{output};

        const auto cmd = "sh -c " + quote_script(script);
        auto args = options;
        args.insert(args.end(), {"-p", port, "-l", user, "--", host, cmd});
        ChildProcess child(tool("ssh"), args, true, output);
        if (!child.wait(30s) || child.exit_code() != 0)
            throw std::runtime_error(
                "SSH inspection failed; verify host identity, login permission and " "target Linux platform");
        if (fs::file_size(output) > 65536)
            throw std::runtime_error("SSH report too large");
        std::ifstream input(output);
        return Json::parse(input);
    }

    void upload(const fs::path& batch)
    {
        auto args = options;
        args.insert(
            args.end(),
            {"-P",
                port,
                "-b",
                utf8(batch),
                user + "@" + (host.find(':') == std::string::npos ? host : "[" + host + "]")});
        ChildProcess child(tool("sftp"), args, true);
        if (!child.wait(120s) || child.exit_code() != 0)
            throw std::runtime_error(
                "SSH upload failed; check authentication, host identity and target directory permission");
    }
};

// What the dedicated unprivileged SSH account runs before it touches a node's
// directory: the directory and every file must be private, owned and regular,
// and a file is published by exclusive link or verified against what is there.
constexpr std::string_view publication = R"PY(import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import tempfile

stage, root = Path(sys.argv[1]).absolute(), Path(sys.argv[2])

def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)

def directory(path):
    if path.is_symlink():
        raise ValueError('Installation directory cannot be a symlink')
    if not path.exists():
        directory(path.parent)
        path.mkdir(mode=0o700)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode):
        raise ValueError('Installation path is not a directory')
    if path == root.parent or root in path.parents or path == root:
        if info.st_uid != os.geteuid() or info.st_mode & 0o077:
            raise ValueError('Installation directory must be private to this user')
    sync_directory(path)
    sync_directory(path.parent)

def open_owned(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    source = os.fdopen(fd, 'rb')
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_nlink != 1:
        source.close()
        raise ValueError('Installation file must be an owned regular file')
    return source

def verify(path, digest, mode):
    with open_owned(path) as source:
        if stat.S_IMODE(os.fstat(source.fileno()).st_mode) != mode or hashlib.file_digest(source, 'sha256').hexdigest() != digest:
            raise ValueError('Existing installation differs; original files preserved')
        os.fsync(source.fileno())
    sync_directory(path.parent)

def publish(source_path, target, mode, expected=None):
    with open_owned(source_path) as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
        if expected is not None and digest != expected:
            raise ValueError('Uploaded Agent integrity check failed')
        try:
            verify(target, digest, mode)
        except FileNotFoundError:
            pass
        else:
            return
        source.seek(0)
        fd, temporary = tempfile.mkstemp(prefix='.asterion-publish-', dir=target.parent)
        try:
            with os.fdopen(fd, 'wb') as output:
                os.fchmod(output.fileno(), mode)
                copied = hashlib.sha256()
                while block := source.read(1024 * 1024):
                    output.write(block)
                    copied.update(block)
                if copied.hexdigest() != digest:
                    raise ValueError('Uploaded file changed during publication')
                output.flush()
                os.fsync(output.fileno())
            try:
                os.link(temporary, target, follow_symlinks=False)
            except FileExistsError:
                verify(target, digest, mode)
        finally:
            os.unlink(temporary)
        sync_directory(target.parent)
)PY";

unsigned short port(const Json& p, const char* key)
{
    const auto value = p.at(key).get<std::string>();
    if (!std::regex_match(value, std::regex("[0-9]{1,5}")))
        throw std::invalid_argument("invalid port");
    const auto n = std::stoul(value);
    if (!n || n > 65535)
        throw std::invalid_argument("invalid port");
    return static_cast<unsigned short>(n);
}

NodeEndpoint endpoint(const fs::path& root, const Json& j)
{
    return {
        j.at("id"),
        j.at("host"),
        j.at("port").get<unsigned short>(),
        {utf8(root / "ca.crt"), utf8(root / "client.crt"), utf8(root / "client.key")}
    };
}

} // namespace

namespace {

fs::path firewall_record(const Json& p)
{
    const auto id = p.at("id").get<std::string>();
    valid_id(id);
    return node_enrollment_directory() / ".firewall"
        / (id + "-" + std::to_string(port(p, "firewall_port")) + ".json");
}

Json owned_firewall(const Json& p)
{
    const auto file = firewall_record(p);
    if (!fs::exists(file))
        return nullptr;
    if (fs::is_symlink(file.parent_path()) || fs::is_symlink(file) || fs::file_size(file) > 65536)
        throw std::invalid_argument("invalid firewall ownership record");
    std::ifstream input(file);
    auto record = Json::parse(input);
    for (const char* field : {"host", "username", "ssh_port"})
        if (record.at(field) != p.at(field))
            throw std::invalid_argument(
                "firewall rule belongs to a different machine configuration");
    return record;
}

Json probe_firewall(const Json& p, Ssh& ssh, const std::string& os)
{
    (void)p;
    auto report = ssh.report(node::firewall_inspection(os));
    node::validate_firewall_source(report.at("source").get<std::string>());
    if (report.size() != 4 || report.at("os") != os || !report.at("backend").is_string()
        || !report.at("state").is_string())
        throw std::invalid_argument("invalid firewall inspection response");
    return report;
}

} // namespace

Json registered_node_inventory()
{
    Json result{{"names", Json::array()}};
    try {
        const auto root = node_enrollment_directory();
        if (fs::is_symlink(root))
            throw std::invalid_argument("invalid enrollment root");
        if (!fs::exists(root))
            return result;
        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(root)) {
            const auto name = entry.path().filename().string();
            if (name.starts_with('.') || name == "enrollment.lock" || name == "ssh-keys.lock")
                continue;
            valid_id(name);
            if (names.size() >= 1000)
                throw std::invalid_argument("registered node inspection limit exceeded");
            if (!fs::is_directory(entry.path()) || fs::is_symlink(entry.path()))
                throw std::invalid_argument("invalid enrollment path");
            names.push_back(name);
        }
        std::ranges::sort(names);
        result["names"] = names;
    } catch (const std::exception& e) {
        result["error"] = e.what();
    }
    return result;
}

Json prepare_ssh_key(const std::string& id)
{
    valid_id(id);
    const auto root = node_enrollment_directory();
    if (fs::is_symlink(root))
        throw std::invalid_argument("invalid enrollment root");
    create_directories_durably(root);
    FileLock lock(root, "ssh-keys.lock");
    const auto base = root / ".ssh-keys";
    if (!fs::exists(base) && !fs::is_symlink(base))
        private_directory(base);
    validate_key_path(base, true);
    create_directories_durably(base);
    const auto directory = base / id;
    if (!fs::exists(directory) && !fs::is_symlink(directory)) {
        if (fs::exists(root / id / "enrollment.json"))
            throw std::invalid_argument(
                "existing node has no local SSH key; an administrator must " "re-authorize explicitly, no replacement key is generated");
        private_directory(directory);
        try {
            ChildProcess generate(
                tool("ssh-keygen"),
                {"-q",
                    "-t",
                    "ed25519",
                    "-N",
                    "",
                    "-C",
                    "asterion-terminal",
                    "-f",
                    utf8(directory / "identity")});
            if (!generate.wait(30s) || generate.exit_code() != 0)
                throw std::runtime_error(
                    "cannot generate local SSH identity; check OpenSSH installation");
            fs::permissions(
                directory / "identity.pub",
                fs::perms::owner_read | fs::perms::owner_write);
        } catch (...) {
            std::error_code ignored;
            fs::remove_all(directory, ignored);
            throw;
        }
    }
    const auto checked = key_directory(id);
    create_directories_durably(checked);
    validate_key_path(checked / "identity", false);
    const auto public_key = read_key_file(checked / "identity.pub");
    if (!public_key.starts_with("ssh-ed25519 ") || public_key.find("PRIVATE") != std::string::npos)
        throw std::invalid_argument("invalid managed public key");
    const auto report = checked / ("public-check-" + unique_process_id());

    struct Cleanup
    {
        fs::path file;

        ~Cleanup()
        {
            std::error_code ignored;
            fs::remove(file, ignored);
        }
    } cleanup{report};

    ChildProcess derive(
        tool("ssh-keygen"),
        {"-y", "-P", "", "-f", utf8(checked / "identity")},
        false,
        report);
    if (!derive.wait(10s) || derive.exit_code() != 0)
        throw std::runtime_error("cannot validate managed SSH identity");
    const auto actual = read_key_file(report);
    std::string type, body, actual_type, actual_body;
    std::istringstream(public_key) >> type >> body;
    std::istringstream(actual) >> actual_type >> actual_body;
    if (type != actual_type || body != actual_body)
        throw std::invalid_argument(
            "managed public/private key mismatch; original files preserved");
    // OpenSSH owns generation. Flush its existing files without rewriting keys,
    // including on an explicit retry after a failed durability barrier.
    sync_file_durably(checked / "identity");
    sync_file_durably(checked / "identity.pub");
    return {{"id", id}, {"public_key", public_key}};
}

Json inspect_node_firewall(const Json& p)
{
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
            owned.is_null() ? "asterion-" + unique_process_id()
                            : owned.at("rule").get<std::string>()},
        {"can_apply",
            observed.at("state") == "active" && observed.at("backend") == "ufw"
                && (action == "allow" || !owned.is_null())},
        {"owned", !owned.is_null()},
        {"verification", "not_checked"}
    };
}

Json change_node_firewall(const Json& p, const Json& plan)
{
    SshIdentity identity(selected_ssh_key(p));
    Ssh ssh(p, identity.file());
    const auto os = plan.at("os").get<std::string>();
    const auto now = probe_firewall(p, ssh, os);
    if (!plan.at("can_apply").get<bool>() || now.at("source") != plan.at("observed_source")
        || now.at("backend") != plan.at("backend") || now.at("state") != "active")
        throw std::invalid_argument("firewall inspection changed; inspect and confirm again");
    const auto file = firewall_record(p);
    const bool remove = plan.at("action") == "remove";
    if (fs::is_symlink(node_enrollment_directory()) || fs::is_symlink(file.parent_path()))
        throw std::invalid_argument("invalid firewall state directory");
    create_directories_durably(file.parent_path());
    fs::permissions(file.parent_path(), fs::perms::owner_all);
    FileLock ownership(file.parent_path(), "firewall.lock");
    const auto current = owned_firewall(p);
    if (!current.is_null()
        && (current.at("rule") != plan.at("rule") || current.at("source") != plan.at("source")))
        throw std::invalid_argument("owned firewall rule changed; inspect again");
    if (remove) {
        const auto owned = owned_firewall(p);
        if (owned.is_null() || owned.at("rule") != plan.at("rule")
            || owned.at("source") != plan.at("source"))
            throw std::invalid_argument("no matching owned firewall rule");
    } else {
        if (fs::is_symlink(node_enrollment_directory()) || fs::is_symlink(file.parent_path()))
            throw std::invalid_argument("invalid firewall state directory");
        create_directories_durably(file.parent_path());
        fs::permissions(file.parent_path(), fs::perms::owner_all);
        Json record = plan;
        for (const char* field : {"host", "username", "ssh_port"})
            record[field] = p.at(field);
        // Persist ownership intent first: failed or lost replies can be inspected/retried without
        // losing the rule identity.
        write(file, record.dump());
    }
    const auto result = ssh.report(
        node::firewall_change(
            os,
            plan.at("source"),
            plan.at("port").get<unsigned short>(),
            plan.at("rule"),
            remove));
    if (result != Json{{"changed", true}})
        throw std::runtime_error("unexpected firewall change response");
    if (remove) {
        fs::remove(file);
        sync_directory(file.parent_path());
    }
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
                auto channel = ipc::TlsChannel::connect(
                    config.host,
                    plan.at("port").get<unsigned short>(),
                    config.tls,
                    3s);
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

namespace {

// The record of a node this machine enrolled.
Json enrollment(const fs::path& root, const std::string& id)
{
    const auto file = root / "enrollment.json";
    if (fs::is_symlink(root) || fs::is_symlink(file) || !fs::is_regular_file(file)
        || fs::file_size(file) > 65536)
        throw std::invalid_argument("node SSH identity is not configured");
    std::ifstream in(file);
    Json j = Json::parse(in);
    if (j.at("id") != id || j.at("os") != "linux")
        throw std::invalid_argument("invalid enrollment");
    return j;
}

// The host is the Linux this build deploys to, reached as the dedicated
// account, with the initializer's current helper.
bool initialized_host(Ssh& ssh)
{
    return ssh.command(
        "test \"$(uname -s)\" = Linux && test \"$(uname -m)\" = " + quote("x86_64")
        + " && test \"$(id -un)\" = asterion && test \"$(sudo -n /usr/local/sbin/asterion_host " "--manage check)\" = asterion_host");
}

} // namespace

NodeEndpoint enrolled_node(const std::string& id)
{
    valid_id(id);
    const auto root = node_enrollment_directory() / id;
    const auto config = enrollment(root, id);
    if (!config.value("installed", true))
        throw std::invalid_argument("remote node is uninstalled");
    return endpoint(root, config);
}

NodeEndpoint enroll_node(ServiceIo& io, const Json& p)
{
    const auto id = p.at("id").get<std::string>();
    valid_id(id);
    const auto agent_port = port(p, "agent_port");
    (void)port(p, "ssh_port");
    SshIdentity identity(selected_ssh_key(p));
    Ssh ssh(p, identity.file());
    if (ssh.user != "asterion" || agent_port < 1024)
        throw std::invalid_argument(
            "Linux nodes use the asterion account created by the initializer; " "the Agent port must be at least 1024");
    const auto detected = ssh.report(
        "set -eu\ntest \"$(uname -s)\" = Linux\ncase \"$(uname -m)\" in x86_64) arch=x86_64;; *) " "exit 3;; esac\nprintf '{\"os\":\"linux\",\"arch\":\"%s\"}\\n' \"$arch\"\n");
    if (detected.size() != 2 || detected.at("os") != "linux")
        throw std::invalid_argument("remote deployment supports Linux only");
    const HostPlatform platform{"linux", detected.at("arch").get<std::string>()};
    const auto binary = bundled_linux_program(platform.arch, "asterion_node_agent");
    const auto digest = sha256_file(binary);
    if (!initialized_host(ssh))
        throw std::runtime_error(
            "SSH verification failed; check host identity, authentication, Linux " "platform and host initialization");
    const auto base = node_enrollment_directory();
    if (fs::is_symlink(base))
        throw std::invalid_argument("invalid enrollment root");
    create_directories_durably(base);
    fs::permissions(base, fs::perms::owner_all);
    FileLock lock(base, "enrollment.lock");
    const auto root = base / id;
    Json config{
        {"id", id},
        {"host", ssh.host},
        {"port", agent_port},
        {"artifact", digest},
        {"version", ASTERION_PRODUCT_VERSION},
        {"os", platform.os},
        {"arch", platform.arch}
    };
    if (fs::exists(root)) {
        if (fs::is_symlink(root))
            throw std::invalid_argument("invalid enrollment path");
        std::ifstream in(root / "enrollment.json");
        Json previous = Json::parse(in);
        const bool reinstall = !previous.value("installed", true);
        previous.erase("installed");
        previous.erase("version");
        if (reinstall)
            previous["artifact"] = config.at("artifact");
        auto expected = config;
        expected.erase("version");
        if (previous != expected)
            throw std::invalid_argument(
                "node name is used by another installation; its identity and services are not replaced");
        if (reinstall) {
            if (!fs::exists(root / "client.key"))
                create_node_identity(root, ssh.host);
            write(root / "enrollment.json", config.dump());
        }
        try {
            auto existing = AgentClient::open(io, endpoint(root, config)).get();
            return endpoint(root, config);
        } catch (const std::exception&) {
        }
    } else {
        create_directories_durably(root);
        fs::permissions(root, fs::perms::owner_all);
        create_node_identity(root, ssh.host);
        write(root / "enrollment.json", config.dump());
    }
    save_node_ssh_profile(p);
    const auto stage = ".asterion-install-" + unique_process_id();
    if (!ssh.command("umask 077; mkdir " + quote(stage)))
        throw std::runtime_error("cannot create private SSH installation staging directory");
    const auto destination = "/var/lib/asterion/nodes/" + id;
    const auto bind = ssh.host.find(':') == std::string::npos ? "0.0.0.0" : "::";
    write(root / "install.json", config.dump());
    std::string script = "#!/bin/sh\nset -eu\numask 077\nstage=" + quote(stage)
        + "\nroot=" + quote(destination) + "\n/usr/bin/python3 - \"$stage\" \"$root\" "
        + quote(digest) + " <<'ASTERION_PUBLISH'\n";
    // This runs as the dedicated unprivileged SSH account. Only after every
    // immutable prerequisite and install record is durable may sudo enable a unit.
    script += publication;
    script += R"PY(

directory(root)
# Reject another installation before publishing any prerequisite.
if (root / 'install.json').exists() or (root / 'install.json').is_symlink():
    with open_owned(stage / 'install.json') as source:
        verify(root / 'install.json', hashlib.file_digest(source, 'sha256').hexdigest(), 0o600)
directory(root / 'state')
if not (root / 'install.json').exists():
    directory(root / 'state' / 'config')
    for name in ('task.json', 'logging.json'):
        target = root / 'state' / 'config' / name
        if target.exists() or target.is_symlink():
            continue
        fd, temporary = tempfile.mkstemp(prefix='.config-', dir=target.parent)
        try:
            with os.fdopen(fd, 'wb') as output, open_owned(stage / name) as source:
                output.write(source.read())
                output.flush()
                os.fsync(output.fileno())
            os.link(temporary, target, follow_symlinks=False)
        finally:
            os.unlink(temporary)
        sync_directory(target.parent)
publish(stage / 'agent', root / 'asterion_node_agent', 0o700, sys.argv[3])
for name in ('ca.crt', 'server.crt', 'server.key'):
    publish(stage / name, root / name, 0o600)
publish(stage / 'install.json', root / 'install.json', 0o600)
receipt = root / 'maintenance-operation.json'
if receipt.exists():
    completed = json.loads(receipt.read_text())
    if completed.get('action') != 'uninstall' or not completed.get('complete'):
        raise ValueError('Finish the pending node maintenance before installation')
    (root / 'state' / 'maintenance.json').unlink(missing_ok=True)
    receipt.unlink()
    sync_directory(root / 'state')
    sync_directory(root)
)PY";
    script += "ASTERION_PUBLISH\n";
    script += "sudo -n /usr/local/sbin/asterion_host --manage install " + quote(id) + " "
        + std::to_string(agent_port) + " " + quote(bind) + "\n";
    write(root / "install", script);
    std::string batch;
    for (const auto* file : {"ca.crt", "server.crt", "server.key", "install.json", "install"})
        batch +=
            "put " + batch_quote(utf8(root / file)) + " " + batch_quote(stage + "/" + file) + "\n";
    for (const auto* name : {"task.json", "logging.json"})
        batch += "put " + batch_quote(utf8(node_configuration_templates() / name)) + " "
            + batch_quote(stage + "/" + name) + "\n";
    batch += "put " + batch_quote(utf8(binary)) + " " + batch_quote(stage + "/agent") + "\n";
    write(root / "upload.batch", batch);
    try {
        ssh.upload(root / "upload.batch");
        const auto execute = "sh " + quote(stage + "/install");
        if (!ssh.command(execute, 60s))
            throw std::runtime_error(
                "system service installation incomplete; local identity kept and existing remote " "installation untouched, check the target service and permissions");
    } catch (...) {
        ssh.command("rm -rf -- " + quote(stage));
        throw;
    }
    ssh.command("rm -rf -- " + quote(stage));
    const auto config_endpoint = endpoint(root, config);
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    for (;;) {
        try {
            auto node = AgentClient::open(io, config_endpoint).get();
            const auto status = node->status().get();
            if (status.at("health").at("os") != platform.os
                || status.at("health").at("arch") != platform.arch)
                throw std::invalid_argument("installed Agent platform mismatch");
            return config_endpoint;
        } catch (const Error&) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error(
                    "Agent installed but its TCP/mTLS heartbeat is unreachable; check " "the management port firewall, then connect without reinstalling");
            std::this_thread::sleep_for(500ms);
        }
    }
}

// Gives an enrolled node a new mTLS identity over SSH and leaves everything
// else it has as it is. The CA key is never kept, so no certificate of the old
// set can be extended: every role is issued again. The node's three files are
// replaced, its Agent is ended by the account it runs as and started again by
// the fixed unit, and then this machine's files follow. From the moment the
// new set exists it waits in `next` until both sides use it; running this
// again finishes whatever an interruption left, with that same set.
NodeEndpoint replace_node_identity(ServiceIo& io, const Json& p)
{
    const auto id = p.at("id").get<std::string>();
    valid_id(id);
    (void)port(p, "ssh_port");
    SshIdentity identity(selected_ssh_key(p));
    Ssh ssh(p, identity.file());
    if (ssh.user != "asterion")
        throw std::invalid_argument(
            "Linux nodes use the asterion account created by the initializer");
    const auto base = node_enrollment_directory();
    if (fs::is_symlink(base) || !fs::is_directory(base))
        throw std::invalid_argument("node SSH identity is not configured");
    FileLock lock(base, "enrollment.lock");
    const auto root = base / id;
    const auto config = enrollment(root, id);
    if (config.at("host") != ssh.host)
        throw std::invalid_argument(
            "node name is used by another installation; its identity and services are not replaced");
    const auto next = root / "next";
    if (fs::is_symlink(next))
        throw std::invalid_argument("invalid enrollment path");
    const auto answers = [&](const fs::path& directory) {
        try {
            (void)AgentClient::open(io, endpoint(directory, config)).get()->status().get();
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    // An interruption after the node took the new set leaves only this
    // machine's half: the node is not stopped a second time.
    if (!fs::exists(next) || !answers(next)) {
        if (!initialized_host(ssh))
            throw std::runtime_error(
                "SSH verification failed; check host identity, authentication, " "Linux platform and host initialization");
        if (!fs::exists(next)) {
            // Whole or absent: the set is issued aside and renamed into place.
            const auto draft = root / "next.draft";
            fs::remove_all(draft);
            create_directories_durably(draft);
            fs::permissions(draft, fs::perms::owner_all);
            create_node_identity(draft, ssh.host);
            fs::rename(draft, next);
            sync_directory(root);
        }
        const auto stage = ".asterion-install-" + unique_process_id();
        if (!ssh.command("umask 077; mkdir " + quote(stage)))
            throw std::runtime_error("cannot create private SSH installation staging directory");
        const auto bind = ssh.host.find(':') == std::string::npos ? "0.0.0.0" : "::";
        std::string script = "#!/bin/sh\nset -eu\numask 077\nstage=" + quote(stage)
            + "\nroot=" + quote("/var/lib/asterion/nodes/" + id)
            + "\nunit=" + quote("asterion-node-agent-" + id + ".service")
            + "\n/usr/bin/python3 - \"$stage\" \"$root\" <<'ASTERION_PUBLISH'\n";
        script += publication;
        script += R"PY(
directory(root)
# Only the installation this Terminal enrolled is given the identity.
with open_owned(stage / 'install.json') as source:
    verify(root / 'install.json', hashlib.file_digest(source, 'sha256').hexdigest(), 0o600)
# Every new file is whole and durable beside the old one before any is replaced.
staged = []
try:
    for name in ('ca.crt', 'server.crt', 'server.key'):
        with open_owned(stage / name) as source:
            fd, temporary = tempfile.mkstemp(prefix='.asterion-identity-', dir=root)
            staged.append((temporary, root / name))
            with os.fdopen(fd, 'wb') as output:
                os.fchmod(output.fileno(), 0o600)
                output.write(source.read())
                output.flush()
                os.fsync(output.fileno())
    while staged:
        os.replace(*staged[0])
        staged.pop(0)
finally:
    for temporary, _ in staged:
        os.unlink(temporary)
sync_directory(root)
)PY";
        script += "ASTERION_PUBLISH\n";
        // The Agent reads its identity when it starts. Ending it stops the node's
        // services with it; whatever started it again, it starts on the new files.
        script +=
            "pid=$(/usr/bin/systemctl show --property MainPID --value \"$unit\")\n" "if test \"$pid\" != 0; then\n" "  kill -TERM \"$pid\"\n" "  waited=0\n" "  while kill -0 \"$pid\" 2>/dev/null; do\n" "    test \"$waited\" -lt 120\n" "    sleep 1\n" "    waited=$((waited + 1))\n" "  done\n" "fi\n";
        script += "sudo -n /usr/local/sbin/asterion_host --manage install " + quote(id) + " "
            + std::to_string(config.at("port").get<unsigned short>()) + " " + quote(bind) + "\n";
        write(next / "install.json", config.dump());
        write(next / "replace", script);
        std::string batch;
        for (const auto* file : {"ca.crt", "server.crt", "server.key", "install.json", "replace"})
            batch += "put " + batch_quote(utf8(next / file)) + " " + batch_quote(stage + "/" + file)
                + "\n";
        write(next / "upload.batch", batch);
        try {
            ssh.upload(next / "upload.batch");
            if (!ssh.command("sh " + quote(stage + "/replace"), 180s))
                throw std::runtime_error(
                    "node identity replacement did not complete and services on the node may be " "stopped; replace the identity again to continue");
        } catch (...) {
            ssh.command("rm -rf -- " + quote(stage));
            throw;
        }
        ssh.command("rm -rf -- " + quote(stage));
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!answers(next)) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error(
                    "the node has its new identity but its Agent does not answer with it; check the " "management port firewall, then replace the identity again to finish");
            std::this_thread::sleep_for(500ms);
        }
    }
    for (const auto* file :
        {"ca.crt",
            "client.crt",
            "client.key",
            "trading-client.crt",
            "trading-client.key",
            "server.crt",
            "server.key"}) {
        std::ifstream input(next / file, std::ios::binary);
        const std::string content{std::istreambuf_iterator<char>(input), {}};
        if (content.empty())
            throw std::runtime_error("cannot read the new node identity");
        write(root / file, content);
    }
    fs::remove_all(next);
    sync_directory(root);
    return endpoint(root, config);
}

namespace {

Json read_record(const fs::path& file)
{
    if (fs::is_symlink(file) || fs::file_size(file) > 1024 * 1024)
        throw std::invalid_argument("invalid node maintenance record: " + file.string());
    std::ifstream input(file);
    return Json::parse(input);
}

fs::path maintenance_file()
{
    return node_enrollment_directory() / ".maintenance.json";
}

Json ssh_profile(const std::string& id, const std::vector<Json>& supplied)
{
    auto p = read_record(node_enrollment_directory() / id / "ssh.json");
    p["private_key"] = "";
    for (const auto& item : supplied)
        if (item.at("id") == id)
            p["private_key"] = item.value("private_key", "");
    return p;
}

void host_operation(Ssh& ssh, const std::string& id, const std::string& action)
{
    if (!ssh.command(
            "sudo -n /usr/local/sbin/asterion_host --manage " + action + " " + quote(id),
            90s))
        throw std::runtime_error(
            "remote host operation failed: " + id + " / " + action
            + "; inspect systemctl status asterion-node-agent-" + id + ".service; "
            + ssh.last_error);
}

void check_maintenance_host(Ssh& ssh)
{
    const auto capabilities =
        ssh.report("sudo -n /usr/local/sbin/asterion_host --manage capabilities");
    for (const auto* action : {"install", "stop", "start", "remove"})
        if (std::find(capabilities.begin(), capabilities.end(), Json(action)) == capabilities.end())
            throw std::runtime_error(
                "remote host initialization must be updated by its administrator");
}

std::vector<std::string> candidate_files(const fs::path& folder, const std::string& version)
{
    if (!folder.is_absolute() || fs::is_symlink(folder))
        throw std::invalid_argument("invalid candidate Linux resource directory");
    const auto manifest = read_record(folder / "manifest.json");
    if (manifest.at("product_version") != version || manifest.at("os") != "linux"
        || manifest.at("arch") != "x86_64")
        throw std::invalid_argument("candidate Linux resource version mismatch");
    std::vector<std::string> names{
        "asterion_node_agent",
        "asterion_trading",
        "asterion_market_data",
        "asterion_data_service",
        "asterion_task_service",
        "ctp-md.so",
        "ctp-trader.so",
        "plugins/asterion_tushare.so",
        "plugins/asterion_shfe.so",
        "plugins/asterion_order_limits.so"
    };
    for (const auto& name : names) {
        const auto file = folder / name;
        if (fs::is_symlink(file) || fs::is_symlink(file.parent_path()) || !fs::is_regular_file(file)
            || sha256_file(file) != manifest.at("files").at(name).get<std::string>())
            throw std::invalid_argument("candidate Linux resource verification failed: " + name);
    }
    return names;
}

std::string remote_stage(const std::string& id, const std::string& operation)
{
    return "/var/lib/asterion/.maintenance-" + id + "-" + operation;
}

void upload_maintenance(
    Ssh& ssh,
    const std::string& id,
    const std::string& operation,
    const fs::path& folder,
    const std::vector<std::string>& files)
{
    const auto stage = remote_stage(id, operation);
    if (!ssh.command("umask 077; mkdir -p " + quote(stage + "/plugins")))
        throw std::runtime_error("cannot prepare remote maintenance directory: " + id);
    const auto root = node_enrollment_directory() / id;
    write(root / "maintain.py", std::string(remote_maintenance_script));
    node::proto::ServicePrograms programs;
    programs.set_version(1);
    auto add = [&](node::proto::ServiceKind kind, const std::string& executable) {
        auto* service = programs.add_services();
        service->set_kind(kind);
        service->set_executable(stage + "/" + executable);
        if (kind == node::proto::MARKET_DATA)
            service->set_provider(stage + "/ctp-md.so");
        if (kind == node::proto::MARKET_DATA || kind == node::proto::LIVE_TRADING)
            service->set_catalog(stage + "/ctp-trader.so");
        if (kind == node::proto::TASK_SERVICE) {
        }
    };
    add(node::proto::MARKET_DATA, "asterion_market_data");
    add(node::proto::LIVE_TRADING, "asterion_trading");
    add(node::proto::DATA_SERVICE, "asterion_data_service");
    add(node::proto::TASK_SERVICE, "asterion_task_service");
    for (const auto& file : files)
        if (file.starts_with("plugins/"))
            programs.add_bundled_plugins(stage + "/" + file);
    write(root / "programs.pb", programs.SerializeAsString());
    std::string batch;
    for (const auto* name : {"maintain.py", "programs.pb"})
        batch +=
            "put " + batch_quote(utf8(root / name)) + " " + batch_quote(stage + "/" + name) + "\n";
    for (const auto& name : files)
        batch += "put " + batch_quote(utf8(folder / name)) + " " + batch_quote(stage + "/" + name)
            + "\n";
    write(root / "maintenance.batch", batch);
    ssh.upload(root / "maintenance.batch");
}

void publish_maintenance(
    Ssh& ssh,
    const Json& config,
    const std::string& operation,
    const std::string& action,
    const std::string& version,
    bool remove)
{
    const auto id = config.at("id").get<std::string>();
    const auto stage = remote_stage(id, operation);
    const auto root = "/var/lib/asterion/nodes/" + id;
    // Keep command errors in the Manager response and Agent's own process log.
    const auto script = "python3 " + quote(stage + "/maintain.py") + " " + quote(root) + " "
        + quote(stage) + " " + quote(action) + " " + quote(operation) + " " + quote(version)
        + (remove ? " false " : " true ") + std::to_string(config.at("port").get<unsigned>()) + " "
        + quote(
            config.at("host").get<std::string>().find(':') == std::string::npos ? "0.0.0.0" : "::");
    if (!ssh.command(script, 5min))
        throw std::runtime_error(
            "remote maintenance failed: " + id + "; Agent log directory: " + root + "/state/logs; "
            + ssh.last_error);
}

} // namespace

void save_node_ssh_profile(const Json& p)
{
    const auto id = p.at("id").get<std::string>();
    valid_id(id);
    const auto root = node_enrollment_directory() / id;
    const auto config = enrollment(root, id);
    if (p.at("host") != config.at("host") || p.at("username") != "asterion")
        throw std::invalid_argument("SSH profile does not match the registered node");
    (void)port(p, "ssh_port");
    SshIdentity identity(selected_ssh_key(p));
    Ssh ssh(p, identity.file());
    Json profile;
    for (const auto* field : {"id", "host", "username", "ssh_port", "known_hosts", "key_source"})
        profile[field] = p.at(field);
    write(root / "ssh.json", profile.dump());
}

bool remote_maintenance_pending()
{
    return fs::exists(maintenance_file());
}

Json remote_maintenance_view()
{
    Json result{{"nodes", Json::array()}, {"pending", nullptr}};
    const auto inventory = registered_node_inventory();
    if (inventory.contains("error"))
        throw std::runtime_error(inventory.at("error").get<std::string>());
    for (const auto& name : inventory.at("names")) {
        const auto id = name.get<std::string>();
        const auto root = node_enrollment_directory() / id;
        auto config = enrollment(root, id);
        config["ssh_configured"] = fs::exists(root / "ssh.json");
        if (config["ssh_configured"] == true)
            config["ssh"] = read_record(root / "ssh.json");
        config["directory"] = "/var/lib/asterion/nodes/" + id;
        config["installed"] = config.value("installed", true);
        result["nodes"].push_back(std::move(config));
    }
    if (remote_maintenance_pending())
        result["pending"] = read_record(maintenance_file());
    return result;
}

Json maintain_remote_nodes(
    ServiceIo& io,
    const std::string& action,
    const fs::path& resources,
    const std::string& version,
    const std::vector<std::string>& selected,
    bool remove_data,
    const std::vector<Json>& connections)
{
    if (action == "inspect")
        return remote_maintenance_view();
    if (action == "configure") {
        for (const auto& p : connections)
            save_node_ssh_profile(p);
        return remote_maintenance_view();
    }
    if (action != "prepare" && action != "finish" && action != "uninstall")
        throw std::invalid_argument("invalid remote maintenance action");
    if (action == "finish" && version != ASTERION_PRODUCT_VERSION)
        throw std::invalid_argument(
            "upgrade this machine before starting remote candidate programs");
    auto view = remote_maintenance_view();
    std::vector<std::string> ids;
    for (const auto& node : view.at("nodes")) {
        const auto id = node.at("id").get<std::string>();
        if (node.at("installed") == true
            && (action != "uninstall" || selected.empty()
                || std::find(selected.begin(), selected.end(), id) != selected.end()))
            ids.push_back(id);
    }
    Json plan;
    if (remote_maintenance_pending()) {
        plan = read_record(maintenance_file());
        if ((action == "finish" ? "prepare" : action) != plan.at("action").get<std::string>()
            || (action != "uninstall" && version != plan.at("version").get<std::string>())
            || (action == "uninstall"
                && (remove_data != plan.at("remove_data")
                    || selected != plan.at("selection").get<std::vector<std::string>>())))
            throw std::invalid_argument(
                "continue the recorded remote maintenance operation with the same target and scope");
        ids.clear();
        for (const auto& entry : plan.at("nodes"))
            ids.push_back(entry.at("id"));
    } else {
        if (action == "finish" || ids.empty())
            return view;
        if (action == "uninstall")
            for (const auto& id : selected)
                if (std::find(ids.begin(), ids.end(), id) == ids.end())
                    throw std::invalid_argument("selected remote node is not installed: " + id);
        plan = {
            {"operation", unique_process_id()},
            {"action", action},
            {"version", version},
            {"remove_data", remove_data},
            {"selection", selected},
            {"nodes", Json::array()}
        };
        for (const auto& id : ids)
            plan["nodes"].push_back({{"id", id}, {"phase", "pending"}, {"error", ""}});
    }
    const auto operation = plan.at("operation").get<std::string>();
    auto save = [&] {
        write(maintenance_file(), plan.dump());
    };
    std::vector<std::string> files;
    const auto folder = resources / "x86_64";
    if (action == "prepare")
        files = candidate_files(folder, version);
    // Preflight all SSH connections and service states before entering maintenance.
    if (!remote_maintenance_pending()) {
        for (const auto& id : ids) {
            auto p = ssh_profile(id, connections);
            SshIdentity identity(selected_ssh_key(p));
            Ssh ssh(p, identity.file());
            check_maintenance_host(ssh);
            auto node = AgentClient::open(io, enrolled_node(id)).get();
            const auto status = node->status().get();
            for (const auto& service : status.at("health").at("services"))
                if (service.value("pid", 0ULL) || service.value("desired_running", false))
                    throw std::runtime_error(
                        "stop business services before remote maintenance: " + id);
        }
        save();
    }
    // Stage every outstanding node before stopping any Agent. The durable plan
    // owns partial uploads, so a failed transfer is resumed rather than orphaned.
    if (action != "finish") {
        for (auto& entry : plan["nodes"]) {
            if (entry.at("phase") != "pending" && entry.at("phase") != "stopping")
                continue;
            const auto id = entry.at("id").get<std::string>();
            try {
                auto p = ssh_profile(id, connections);
                SshIdentity identity(selected_ssh_key(p));
                Ssh ssh(p, identity.file());
                upload_maintenance(ssh, id, operation, folder, files);
                entry["error"] = "";
                save();
            } catch (const std::exception& error) {
                entry["error"] = error.what();
                save();
                throw;
            }
        }
    }
    for (auto& entry : plan["nodes"]) {
        const auto id = entry.at("id").get<std::string>();
        try {
            const auto completed_phase = entry.at("phase").get<std::string>();
            if (completed_phase == "complete"
                || (action != "finish" && completed_phase == "prepared"))
                continue;
            auto p = ssh_profile(id, connections);
            SshIdentity identity(selected_ssh_key(p));
            Ssh ssh(p, identity.file());
            auto config = enrollment(node_enrollment_directory() / id, id);
            auto phase = entry.at("phase").get<std::string>();
            if (action == "finish") {
                if (phase != "prepared" && phase != "starting")
                    throw std::runtime_error("remote program preparation is incomplete: " + id);
                entry["phase"] = "starting";
                save();
                host_operation(ssh, id, "start");
                auto node = AgentClient::open(io, enrolled_node(id)).get();
                const auto status = node->status().get();
                if (status.at("health").at("version") != version)
                    throw std::runtime_error("remote Agent target version differs: " + id);
                if (status.at("health").value("maintenance", false))
                    node->maintenance(false, operation, status.at("health").at("instance_id"))
                        .get();
                config["version"] = version;
                write(node_enrollment_directory() / id / "enrollment.json", config.dump());
                entry["phase"] = "complete";
                entry["error"] = "";
                save();
                ssh.command("rm -rf -- " + quote(remote_stage(id, operation)));
                continue;
            }
            if (phase == "pending") {
                auto node = AgentClient::open(io, enrolled_node(id)).get();
                const auto status = node->status().get();
                node->maintenance(true, operation, status.at("health").at("instance_id")).get();
                entry["phase"] = "stopping";
                save();
            }
            if (phase != "removing")
                host_operation(ssh, id, "stop");
            if (phase != "removing")
                publish_maintenance(ssh, config, operation, action, version, remove_data);
            if (action == "uninstall") {
                entry["phase"] = "removing";
                save();
                host_operation(ssh, id, "remove");
                config["installed"] = false;
                if (remove_data) {
                    for (const auto* name :
                        {"ca.crt", "client.crt", "client.key", "server.crt", "server.key"})
                        fs::remove(node_enrollment_directory() / id / name);
                }
                write(node_enrollment_directory() / id / "enrollment.json", config.dump());
                entry["phase"] = "complete";
                ssh.command("rm -rf -- " + quote(remote_stage(id, operation)));
            } else
                entry["phase"] = "prepared";
            entry["error"] = "";
            save();
        } catch (const std::exception& error) {
            entry["error"] = error.what();
            save();
            throw;
        }
    }
    if (action == "finish" || action == "uninstall") {
        fs::remove(maintenance_file());
        sync_directory(node_enrollment_directory());
    }
    return remote_maintenance_view();
}

} // namespace asterion::terminal
