#pragma once
#include <asterion/research.pb.h>
#include <asterion/task.pb.h>
#include "timing.hpp"
#include <chrono>
#include <thread>
#include <stdexcept>

namespace asterion::testing_support {

template <class Call>
task::proto::Task wait_task(
    Call&& call,
    const std::string& id,
    std::chrono::seconds timeout = std::chrono::seconds(60))
{
    const auto deadline = std::chrono::steady_clock::now() + bound(timeout);
    research::proto::ResearchRequest request;
    request.mutable_metadata_get()->set_id(id);
    for (;;) {
        const auto response = call(request);
        if (response.has_error())
            throw std::runtime_error(response.error().message());
        const auto& task = response.task();
        if (task.state() != task::proto::QUEUED && task.state() != task::proto::RUNNING
            && task.state() != task::proto::CANCEL_REQUESTED
            && task.state() != task::proto::PUBLISHING)
            return task;
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("task did not finish: " + id + " " + task.ShortDebugString());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace asterion::testing_support
