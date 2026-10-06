#pragma once
#include <asterion/kernel/polled_task.hpp>
#include <functional>
namespace asterion::agent {
// Runs blocking file or process work on the Agent's bounded operations pool
// and resumes the caller on the Agent's state owner.
using BlockingWork = std::function<PolledTask<void>(std::function<void()>)>;
} // namespace asterion::agent
