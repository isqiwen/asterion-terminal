#pragma once
#include "node_client.hpp"
namespace asterion::terminal {
// Names only; never reads identities, contacts hosts or creates directories.
Json registered_node_inventory();
Json prepare_ssh_key(const std::string& id);
Json inspect_node_firewall(const Json& parameters);
Json change_node_firewall(const Json& parameters, const Json& plan);
NodeEndpoint enroll_node(ServiceIo&, const Json& parameters);
NodeEndpoint enrolled_node(const std::string& id);
void create_node_identity(const std::filesystem::path& directory, const std::string& host);
} // namespace asterion::terminal
