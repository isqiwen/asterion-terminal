#pragma once
#include "agent_client.hpp"

namespace asterion::terminal {

// Names only; never reads identities, contacts hosts or creates directories.
Json registered_node_inventory();
Json prepare_ssh_key(const std::string& id);
Json inspect_node_firewall(const Json& parameters);
Json change_node_firewall(const Json& parameters, const Json& plan);
NodeEndpoint enroll_node(ServiceIo&, const Json& parameters);
NodeEndpoint enrolled_node(const std::string& id);
Json remote_maintenance_view();
bool remote_maintenance_pending();
void save_node_ssh_profile(const Json& parameters);
Json maintain_remote_nodes(
    ServiceIo&,
    const std::string& action,
    const std::filesystem::path& resources,
    const std::string& version,
    const std::vector<std::string>& nodes,
    bool remove_data,
    const std::vector<Json>& connections);
// Issues an enrolled node's whole mTLS identity again over SSH: the node's
// Agent and services stop and start on it, and their data is not touched.
NodeEndpoint replace_node_identity(ServiceIo&, const Json& parameters);
void create_node_identity(const std::filesystem::path& directory, const std::string& host);

} // namespace asterion::terminal
