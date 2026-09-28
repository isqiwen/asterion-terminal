#pragma once
#include "service_endpoint.hpp"
#include <asterion/foundation/serialization.hpp>
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <filesystem>
#include <memory>
namespace asterion::terminal {
struct NodeEndpoint {
  std::string id, host;
  std::uint16_t port;
  ipc::TlsIdentity tls;
  std::string endpoint{};
};
NodeEndpoint local_node();
Json local_node_program_status();
NodeEndpoint upgrade_local_node(const std::string& expected);
class NodeClient {
public:
  explicit NodeClient(NodeEndpoint endpoint);
  ~NodeClient();
  Json status() const;
  void maintenance(bool enter, const std::string& operation, const std::string& instance);
  ServiceEndpoint service_endpoint(const std::string& service, const std::string& kind = "paper");
  void deploy(const std::filesystem::path& executable, const std::string& os,
              const std::string& arch, const std::string& service, std::uint16_t port,
              const std::string& directory = {}, const std::string& kind = "paper",
              const std::filesystem::path& provider = {}, const std::filesystem::path& worker = {},
              const std::filesystem::path& factor = {}, const std::filesystem::path& data = {});
  void update(const std::filesystem::path& executable, const std::string& os,
              const std::string& arch, const std::string& service, const std::string& revision,
              const std::filesystem::path& provider = {}, const std::filesystem::path& worker = {},
              const std::filesystem::path& factor = {}, const std::filesystem::path& data = {});
  ServiceEndpoint local_session(const std::filesystem::path& directory);
  ServiceEndpoint local_market();
  ServiceEndpoint local_research();
  ServiceEndpoint local_strategy(const std::string& service);
  Json firewall(const std::string& service, const std::string& action,
                const std::string& token = {});
  void action(const std::string& service, const std::string& operation);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::terminal
