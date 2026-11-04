#pragma once
#include <asterion/node.pb.h>
#include "node_snapshot.hpp"
#include "service_endpoint.hpp"
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace asterion::terminal {

// The business services on this machine, each a child process of the
// Terminal: Lab only. Market and Trading are independent installations elsewhere.
//
// Each service is installed in <root>/<program>/ (node_paths.hpp): bin/ holds a
// symbolic link to the program inside the application, data/<service>/ its
// data. A service is registered on first use in
// <root>/terminal/data/services/<service>.json and started whenever the Terminal
// starts. A process that exits or stops answering its heartbeat is restarted
// every five seconds until it runs again. Every service watches the Terminal's
// process (--owner-pid) and exits with it; the destructor stops them.
//
// One Terminal owns a root: a second one fails, naming the lock.
class LocalServices
{
public:
    // lab runs on the interpreter `python` names, once there is one.
    explicit LocalServices(std::function<std::optional<std::filesystem::path>()> python);
    ~LocalServices();
    LocalServices(const LocalServices&) = delete;
    LocalServices& operator=(const LocalServices&) = delete;

    // The one local node: its identity, the services and their health.
    NodeSnapshot view() const;
    // Registers the kind's service when absent, starts it and
    // returns its endpoint once its process runs. The service clients wait for
    // their service themselves.
    std::future<ServiceEndpoint> start(node::proto::ServiceKind kind);
    // The same, but returns only once the service answers its first heartbeat,
    // so its socket accepts connections; fails if it stops first or never
    // answers. For callers that connect to the socket directly (lab).
    std::future<ServiceEndpoint> serving(node::proto::ServiceKind kind);
    // A running service's endpoint.
    std::future<ServiceEndpoint> endpoint(std::string service, node::proto::ServiceKind kind);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace asterion::terminal
