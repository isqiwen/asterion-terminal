#pragma once
#include <cstdint>
#include <string>

namespace asterion::terminal {

std::string local_manager();
void stop_development_manager(const std::string& endpoint, std::uint64_t pid);

} // namespace asterion::terminal
