#pragma once
#include "managed_service.hpp"
#include "node_snapshot.hpp"
#include "service_endpoint.hpp"
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace asterion::terminal {

// The business services on this machine, each a child process of the
// Terminal: market (market-data), Data (historical-data), Research (research)
// and lab. Trading and the recorder are independent installations elsewhere.
//
// Each service is installed in <root>/<program>/ (node_paths.hpp): bin/ holds a
// symbolic link to the program inside the application, data/<service>/ its
// data. What runs is kept in <root>/terminal/data/services/<service>.json
// (kind, whether it should run, its data-source plugins); a service that
// should run is started when the Terminal starts. A process that exits or
// stops answering its heartbeat is restarted, at most three times; a stopped
// service is never started by itself. Every service watches the Terminal's
// process (--owner-pid) and exits with it; the destructor stops them, the
// services that read Data before Data itself.
//
// Data and Research find each other, the market service and their own program
// fingerprint through a discovery socket that answers node::proto status
// requests. It goes when they do (M2.6).
//
// One Terminal owns a root: a second one fails, naming the lock.
class LocalServices
{
public:
    LocalServices();
    ~LocalServices();
    LocalServices(const LocalServices&) = delete;
    LocalServices& operator=(const LocalServices&) = delete;

    // The one local node: its identity, the services and their health.
    NodeSnapshot view() const;
    // Registers the kind's service when absent (with the bundled plugins, or the
    // given selection for Research), marks it to run, starts it and returns its
    // endpoint once its process runs.
    std::future<ServiceEndpoint> start(
        node::proto::ServiceKind kind,
        std::optional<std::vector<std::string>> research_plugins = std::nullopt);
    // A running service's endpoint.
    std::future<ServiceEndpoint> endpoint(std::string service, node::proto::ServiceKind kind);
    // start, stop or restart; a stop is kept across Terminal restarts.
    std::future<void> act(std::string service, std::string action);
    // Replaces a stopped Data or Research service's plugin selection.
    std::future<void> configure_plugins(
        std::string service,
        std::string expected_revision,
        std::vector<std::string> plugins);
    // Data and Research services with their directories, running or not.
    std::vector<HistoryService> history() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace asterion::terminal
