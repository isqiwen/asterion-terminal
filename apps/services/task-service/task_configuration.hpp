#pragma once
#include <filesystem>

namespace asterion::tasks {

// Runtime policy is read once, never created or changed by service startup.
unsigned load_task_concurrency(const std::filesystem::path& path);

} // namespace asterion::tasks
