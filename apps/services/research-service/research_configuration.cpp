#include "research_configuration.hpp"
#include <asterion/foundation/serialization.hpp>
#include <fstream>

namespace asterion::tasks {

unsigned load_task_concurrency(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot read research service configuration: " + path.string());
    const auto value = Json::parse(input, nullptr, false);
    if (!value.is_object() || value.size() != 1 || !value.contains("task_concurrency")
        || !value.at("task_concurrency").is_number_unsigned())
        throw std::invalid_argument("invalid research service configuration: " + path.string());
    const auto limit = value.at("task_concurrency").get<std::uint64_t>();
    if (limit == 0 || limit > 2)
        throw std::invalid_argument("invalid research service configuration: " + path.string());
    return static_cast<unsigned>(limit);
}

} // namespace asterion::tasks
