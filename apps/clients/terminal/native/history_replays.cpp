#include "history_replays.hpp"
#include "paper_record.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <algorithm>
#include <chrono>
#include <map>
#include <tuple>
#include <stdexcept>

namespace asterion::terminal {
namespace {
using namespace std::chrono_literals;
std::pair<bool, bool> owner_usage(const ServiceEndpoint& address, const std::string& directory,
                                  const std::string& id,
                                  std::chrono::steady_clock::time_point deadline) {
  const bool remote = address.endpoint.empty();
  const auto remaining = [&] {
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (duration <= 0ms)
      throw std::runtime_error("replay inspection timed out");
    return std::min(duration, remote ? 2000ms : 1000ms);
  };
  protocol::v1::Request request;
  request.set_version(1);
  request.set_session_id(address.session);
  request.set_mode(protocol::v1::PAPER);
  request.set_correlation_id("history." + unique_process_id());
  request.mutable_history_usage()->set_dataset_id(id);
  const auto exchange = [&](auto channel) {
    channel.send(request.SerializeAsString(), remaining());
    return channel.receive(remaining());
  };
  const auto payload =
      remote
          ? exchange(ipc::TlsChannel::connect(address.host, address.port, address.tls, remaining()))
          : exchange(ipc::Channel::connect(address.endpoint, remaining()));
  protocol::v1::Response response;
  if (!response.ParseFromString(payload))
    throw std::runtime_error("invalid replay usage response");
  protocol::validate_message(response);
  if (response.version() != 1 || response.mode() != protocol::v1::PAPER ||
      response.session_id() != address.session ||
      response.correlation_id() != request.correlation_id())
    throw std::runtime_error("replay usage response identity mismatch");
  if (response.has_error())
    throw_remote_error(response.error().code(), response.error().message());
  if (!response.has_history_usage())
    throw std::runtime_error("invalid replay usage response");
  const auto& usage = response.history_usage();
  const auto path =
      std::filesystem::path(std::u8string(usage.directory().begin(), usage.directory().end()));
  if (usage.dataset_id() != id || usage.dataset_revision().size() != 64 ||
      usage.dataset_revision().find_first_not_of("0123456789abcdef") != std::string::npos ||
      usage.directory().find('\0') != std::string::npos || !path.is_absolute() ||
      (remote ? (!directory.empty() &&
                 path.lexically_normal() != std::filesystem::path(directory).lexically_normal())
              : !std::filesystem::equivalent(path, std::filesystem::path(directory))))
    throw std::runtime_error("replay usage response identity mismatch");
  return {usage.market(), usage.settlement()};
}
} // namespace
Json remote_replay_usage(const std::vector<RemoteReplayOwner>& owners, const std::string& id) {
  Json result{{"checked", 0}, {"references", Json::array()}, {"unavailable", Json::array()}};
  if (owners.size() > 1000) {
    result["error"] = "remote replay inspection limit exceeded";
    return result;
  }
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  for (const auto& owner : owners) {
    try {
      if (owner.state != "running")
        throw std::runtime_error("remote replay service is not running; ledger was not inspected");
      if (!owner.address.endpoint.empty() || owner.address.host.empty() || !owner.address.port)
        throw std::invalid_argument("invalid remote replay service address");
      const auto [market, settlement] = owner_usage(owner.address, owner.directory, id, deadline);
      result["checked"] = result.at("checked").get<unsigned>() + 1;
      if (market || settlement) {
        Json roles = Json::array();
        if (market)
          roles.push_back("market");
        if (settlement)
          roles.push_back("settlement");
        result["references"].push_back(
            {{"name", owner.address.session}, {"roles", std::move(roles)}});
      }
    } catch (const std::exception& e) {
      result["unavailable"].push_back({{"name", owner.address.session}, {"diagnostic", e.what()}});
    }
  }
  return result;
}
Json local_replay_usage(const std::filesystem::path& node, const std::string& id,
                        const std::vector<ReplayOwner>& owners) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  Json result{{"checked", 0}, {"references", Json::array()}, {"unavailable", Json::array()}};
  try {
    for (const auto& directory : {node, node / "accounts", node / "accounts" / "paper"}) {
      if (std::filesystem::is_symlink(directory))
        throw std::invalid_argument("invalid local replay account directory");
      if (!std::filesystem::exists(directory))
        return result;
      if (!std::filesystem::is_directory(directory))
        throw std::invalid_argument("invalid local replay account directory");
    }
    std::vector<std::filesystem::path> accounts;
    for (const auto& entry : std::filesystem::directory_iterator(node / "accounts" / "paper")) {
      if (accounts.size() >= 1000)
        throw std::invalid_argument("local replay account inspection limit exceeded");
      accounts.push_back(entry.path());
    }
    std::map<std::filesystem::path, std::vector<const ReplayOwner*>> owner_index;
    for (const auto& owner : owners)
      owner_index[std::filesystem::weakly_canonical(owner.directory)].push_back(&owner);
    std::ranges::sort(accounts);
    for (const auto& directory : accounts) {
      const auto name = directory.filename().string();
      try {
        if (!std::filesystem::is_directory(directory) || std::filesystem::is_symlink(directory))
          throw std::invalid_argument("invalid local replay account directory");
        const ReplayOwner* owner = nullptr;
        const auto found = owner_index.find(std::filesystem::weakly_canonical(directory));
        if (found != owner_index.end()) {
          if (found->second.size() != 1)
            throw std::runtime_error("multiple services claim the replay account");
          owner = found->second.front();
        }
        bool market = false, settlement = false;
        if (owner) {
          std::tie(market, settlement) = owner_usage({{}, owner->session, 0, {}, owner->endpoint},
                                                     directory.string(), id, deadline);
        } else {
          const auto input = trading::read_paper_input(directory);
          for (const auto& contract : input.contracts()) {
            const auto& data = contract.dataset();
            market |=
                std::ranges::find(data.source_dataset_ids(), id) != data.source_dataset_ids().end();
            settlement |= std::ranges::find(data.settlement_dataset_ids(), id) !=
                          data.settlement_dataset_ids().end();
          }
        }
        result["checked"] = result.at("checked").get<unsigned>() + 1;
        if (market || settlement) {
          Json roles = Json::array();
          if (market)
            roles.push_back("market");
          if (settlement)
            roles.push_back("settlement");
          result["references"].push_back({{"name", name}, {"roles", std::move(roles)}});
        }
      } catch (const std::exception& e) {
        result["unavailable"].push_back({{"name", name}, {"diagnostic", e.what()}});
      }
    }
  } catch (const std::exception& e) {
    result["error"] = e.what();
  }
  return result;
}
} // namespace asterion::terminal
