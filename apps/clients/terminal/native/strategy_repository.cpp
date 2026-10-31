#include "strategy_repository.hpp"
#include <asterion/base/error.hpp>
#include <asterion/system/durable_file.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>

namespace asterion::terminal {
namespace fs = std::filesystem;

namespace {

void validate(const std::string& path)
{
    if (path.empty() || path.size() > 4096 || !fs::path(path).is_absolute()
        || std::any_of(path.begin(), path.end(), [](unsigned char c) {
               return std::iscntrl(c);
           }))
        throw std::invalid_argument("a strategy repository is an absolute directory path");
}

} // namespace

Json StrategyRepository::get() const
{
    if (!fs::exists(file_))
        return nullptr;
    try {
        if (fs::is_symlink(file_) || !fs::is_regular_file(file_) || fs::file_size(file_) > 8192)
            throw std::invalid_argument("not a small regular file");
        std::ifstream input(file_);
        const auto value = parse_json(std::string{std::istreambuf_iterator<char>(input), {}}, 8192);
        require_fields(value, {"path"});
        const auto path = value.at("path").get<std::string>();
        validate(path);
        return {{"path", path}};
    } catch (const std::exception& error) {
        throw std::invalid_argument(
            "invalid research settings " + file_.string() + ": " + error.what());
    }
}

void StrategyRepository::save(const std::string& path) const
{
    validate(path);
    if (!fs::is_directory(path))
        throw std::invalid_argument("the strategy repository is not a directory: " + path);
    create_directories_durably(file_.parent_path());
    replace_file_durably(file_, Json{{"path", path}}.dump());
}

void StrategyRepository::clear() const
{
    if (fs::exists(file_)) {
        fs::remove(file_);
        sync_directory(file_.parent_path());
    }
}

} // namespace asterion::terminal
