#pragma once
#include <filesystem>
#include <string>
namespace asterion::terminal {
std::filesystem::path bundled_linux_program(const std::string &arch,
                                            const std::string &program);
std::string bundled_linux_initializer();
void export_bundled_initializer(const std::filesystem::path &destination);
} // namespace asterion::terminal
