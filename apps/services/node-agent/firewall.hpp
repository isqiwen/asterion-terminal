#pragma once
#include <asterion/foundation/serialization.hpp>
#include <filesystem>
namespace asterion::node {
void validate_firewall_source(const std::string& source);
std::string firewall_inspection(const std::string& os, const std::string& source = {});
std::string firewall_change(const std::string& os, const std::string& source, unsigned short port,
                            const std::string& rule, bool remove);
std::string powershell_command(const std::string& script);
Json run_firewall_script(const std::string& os, const std::string& script);
} // namespace asterion::node
