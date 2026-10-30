#include "recorder_endpoint.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>

namespace asterion::terminal {
namespace fs = std::filesystem;

namespace {

void validate(const std::string& host, unsigned port)
{
    if (host.empty() || host.size() > 253
        || std::any_of(host.begin(), host.end(), [](unsigned char c) {
               return std::isspace(c) || std::iscntrl(c);
           }))
        throw std::invalid_argument("invalid recorder host");
    if (port < 1 || port > 65535)
        throw std::invalid_argument("invalid recorder port");
}

} // namespace

Json RecorderEndpoint::get() const
{
    if (!fs::exists(file_))
        return nullptr;
    try {
        if (fs::is_symlink(file_) || !fs::is_regular_file(file_) || fs::file_size(file_) > 1024)
            throw std::invalid_argument("not a small regular file");
        std::ifstream input(file_);
        const auto value = parse_json(std::string{std::istreambuf_iterator<char>(input), {}}, 1024);
        require_fields(value, {"host", "port"});
        const auto host = value.at("host").get<std::string>();
        const auto port = value.at("port").get<unsigned>();
        validate(host, port);
        return {{"host", host}, {"port", port}};
    } catch (const std::exception& error) {
        throw std::invalid_argument(
            "invalid recorder settings " + file_.string() + ": " + error.what());
    }
}

void RecorderEndpoint::save(const std::string& host, unsigned port) const
{
    validate(host, port);
    create_directories_durably(file_.parent_path());
    replace_file_durably(file_, Json{{"host", host}, {"port", port}}.dump());
}

void RecorderEndpoint::clear() const
{
    if (fs::exists(file_)) {
        fs::remove(file_);
        sync_directory(file_.parent_path());
    }
}

} // namespace asterion::terminal
