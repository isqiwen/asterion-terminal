#pragma once
#include "store.hpp"
#include <nlohmann/json.hpp>

namespace asterion::warehouse::acquisition::values {

using Json = nlohmann::json;
constexpr std::size_t response_limit = 8 * 1024 * 1024;
Json parse(std::string_view);
std::string numeric(const Json&, std::int64_t shift = 0);
warehouse::Cell number(const Json&, int shift = 0);

} // namespace asterion::warehouse::acquisition::values
