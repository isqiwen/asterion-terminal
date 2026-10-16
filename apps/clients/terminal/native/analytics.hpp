#pragma once
#include <filesystem>
#include <vector>
#include <nlohmann/json.hpp>

namespace asterion::terminal {

// Source has already passed TaskClient's result and digest validation.
nlohmann::json
quantstats_report(const nlohmann::json& source, const std::filesystem::path& executable);

nlohmann::json portfolio_report(
    const std::vector<nlohmann::json>& sources,
    const std::string& training_end,
    const std::filesystem::path& executable);

} // namespace asterion::terminal
