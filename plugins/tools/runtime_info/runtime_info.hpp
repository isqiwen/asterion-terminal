#pragma once
#include <asterion/kernel/plugin.hpp>

namespace asterion {
[[nodiscard]] std::unique_ptr<Plugin> make_runtime_info();
}
