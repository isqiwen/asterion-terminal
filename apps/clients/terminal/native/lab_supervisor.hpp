#pragma once
#include "service_endpoint.hpp"
#include <asterion/base/serialization.hpp>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>

namespace asterion::terminal {

// Lab, the Python research service, as a child process of the Terminal. Its
// program data is in <root>/lab/ (program_paths.hpp). Once started it is kept
// running: a process that exits or stops answering its heartbeat is restarted
// every five seconds until it runs again. Lab watches the Terminal's process
// (--owner-pid) and exits with it; the destructor stops it.
//
// One Terminal owns a root: a second one fails, naming the lock.
class LabSupervisor
{
public:
    // Lab runs on the interpreter `python` names, once there is one.
    explicit LabSupervisor(std::function<std::optional<std::filesystem::path>()> python);
    ~LabSupervisor();
    LabSupervisor(const LabSupervisor&) = delete;
    LabSupervisor& operator=(const LabSupervisor&) = delete;

    // Lab as the snapshot shows it; null before it was first started.
    Json view() const;
    // Starts Lab when it is not running and returns its endpoint once it
    // answers its first heartbeat, so its socket accepts connections; fails if
    // it stops first or never answers.
    std::future<ServiceEndpoint> serving();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace asterion::terminal
