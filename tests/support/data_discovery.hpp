#pragma once
#include "local_listener.hpp"
#include <asterion/node.pb.h>
#include <thread>
#include <vector>
#include <utility>
#include <mutex>
#include <cstdint>

namespace asterion::testing_support {

// Isolated Agent observation: no actual management service, account or provider.
class DataDiscovery
{
public:
    DataDiscovery(
        std::string endpoint,
        std::string service,
        std::string data_endpoint,
        std::vector<std::pair<std::string, std::string>> others = {})
        : listener_(std::move(endpoint), 8),
          worker_([this,
                      service = std::move(service),
                      data_endpoint = std::move(data_endpoint),
                      others = std::move(others)](std::stop_token stop) mutable {
              using namespace std::chrono_literals;
              others.insert(others.begin(), {service, data_endpoint});
              while (!stop.stop_requested()) {
                  try {
                      auto peer = listener_.accept(100ms);
                      node::proto::Request request;
                      if (!request.ParseFromString(peer.receive(200ms)))
                          continue;
                      node::proto::Response response;
                      response.set_version(request.version());
                      response.set_correlation_id(request.correlation_id());
                      if (!request.has_status()) {
                          response.mutable_error()->set_code("permission_denied");
                          response.mutable_error()->set_message("read only discovery fixture");
                      } else {
                          auto* status = response.mutable_status();
                          status->set_node_id("fixture-node");
                          status->set_instance_id("fixture-generation");
                          status->set_phase(node::proto::Status::READY);
                          {
                              std::lock_guard lock(mutex_);
                              if (research_pid_) {
                                  auto* research = status->add_services();
                                  research->set_id(research_id_);
                                  research->set_kind(node::proto::RESEARCH);
                                  research->set_state("running");
                                  research->set_pid(research_pid_);
                                  research->set_artifact(research_artifact_);
                              }
                          }
                          for (const auto& [id, endpoint] : others) {
                              auto* data = status->add_services();
                              data->set_id(id);
                              data->set_kind(node::proto::DATA_SERVICE);
                              data->set_state("running");
                              data->set_endpoint(endpoint);
                          }
                      }
                      peer.send(response.SerializeAsString(), 200ms);
                  } catch (const Error&) {
                      // Poll timeouts bound fixture shutdown; no production retry policy.
                  }
              }
          })
    {}

    // Test owner supplies the actual isolated child's PID and executable digest.
    void research(std::string id, std::uint64_t pid, std::string artifact)
    {
        std::lock_guard lock(mutex_);
        research_id_ = std::move(id);
        research_pid_ = pid;
        research_artifact_ = std::move(artifact);
    }

private:
    LocalListener listener_;
    std::mutex mutex_;
    std::string research_id_, research_artifact_;
    std::uint64_t research_pid_ = 0;
    std::jthread worker_;
};

} // namespace asterion::testing_support
