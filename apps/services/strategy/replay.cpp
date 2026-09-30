#include "replay.hpp"
#include "replay_schedule.hpp"
#include <asterion/foundation/decimal.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/strategy.hpp>
#include <asterion/v1/node.pb.h>
#include <stdexcept>
using namespace std::chrono_literals;
namespace asterion::strategy {
namespace {
protocol::v1::Snapshot exchange(const v1::ReplayPlan& plan, protocol::v1::Request request) {
  auto rpc = [&](auto& channel) {
    channel.send(request.SerializeAsString(), 3s);
    protocol::v1::Response response;
    if (!response.ParseFromString(channel.receive(3s)))
      throw std::runtime_error("invalid trading reply");
    protocol::validate_message(response);
    if (response.version() != 1 || response.mode() != protocol::v1::PAPER ||
        response.session_id() != plan.trading_session() ||
        response.correlation_id() != request.correlation_id())
      throw std::runtime_error("strategy trading response identity mismatch");
    if (response.has_error())
      throw_remote_error(response.error().code(), response.error().message());
    if (!response.has_snapshot())
      throw std::runtime_error("strategy requires an initialized paper account");
    static_cast<void>(protocol::decode_snapshot(response.snapshot()));
    return response.snapshot();
  };
  if (!plan.agent_endpoint().empty()) {
    node::v1::Request status;
    status.set_version(1);
    status.set_correlation_id(unique_process_id());
    status.mutable_status();
    auto agent = [&] {
      try {
        return ipc::Channel::connect(plan.agent_endpoint(), 3s);
      } catch (const std::exception& e) {
        throw std::runtime_error(std::string("strategy Agent connection: ") + e.what());
      }
    }();
    agent.send(status.SerializeAsString(), 3s);
    node::v1::Response response;
    if (!response.ParseFromString(agent.receive(3s)))
      throw std::runtime_error("invalid Agent response");
    protocol::validate_message(response);
    if (response.version() != 1 || response.correlation_id() != status.correlation_id() ||
        !response.has_status())
      throw std::runtime_error("Agent is unavailable for strategy replay");
    for (const auto& service : response.status().services())
      if (service.id() == plan.trading_session()) {
        if (service.kind() != node::v1::PAPER_TRADING || service.state() != "running" ||
            !service.desired_running() || service.endpoint().empty())
          throw std::runtime_error("strategy paper service is not running");
        auto channel = [&] {
          try {
            return ipc::Channel::connect(service.endpoint(), 3s);
          } catch (const std::exception& e) {
            throw std::runtime_error(std::string("strategy paper connection: ") + e.what());
          }
        }();
        return rpc(channel);
      }
    throw std::runtime_error("strategy paper service was not found");
  }
  auto channel = ipc::TlsChannel::connect(plan.host(), static_cast<std::uint16_t>(plan.port()),
                                          {plan.tls_ca(), plan.tls_cert(), plan.tls_key()}, 3s);
  return rpc(channel);
}
} // namespace
Replay::Replay(Session& session, TradingCall transport)
    : session_(session), config_(session.config()), call_(std::move(transport)) {
  if (!config_.has_replay())
    throw std::invalid_argument("missing strategy replay plan");
  identity_ =
      sha256_bytes(protocol::decode_replay_plan(config_.replay()).dump() + config_.session_id());
  const auto& plan = config_.replay();
  schedule_ = std::make_shared<PaperReplaySchedule>(protocol::instrument(plan.dataset().contract()),
                                                    protocol::dataset_bars(plan.dataset()),
                                                    protocol::dataset_days(plan.dataset()));
  if (!call_)
    call_ = [plan = config_.replay()](protocol::v1::Request r) {
      return exchange(plan, std::move(r));
    };
}
protocol::v1::Snapshot Replay::request(protocol::v1::Request value) {
  value.set_version(1);
  value.set_mode(protocol::v1::PAPER);
  value.set_session_id(config_.replay().trading_session());
  value.set_correlation_id(unique_process_id());
  auto snapshot = call_(std::move(value));
  static_cast<void>(protocol::decode_snapshot(snapshot));
  if (snapshot.recovery_required())
    throw std::runtime_error("strategy trading account requires recovery");
  const auto& plan = config_.replay();
  if (!snapshot.has_replay())
    throw std::invalid_argument("strategy account has no day-end settlement binding");
  {
    const auto cursor = snapshot.cursor();
    if (cursor > schedule_->size())
      throw std::invalid_argument("invalid scheduled replay cursor");
    const auto settled = snapshot.replay().settled_days();
    if (!cursor ? settled != 0
                : (settled < schedule_->event(cursor - 1).day ||
                   settled > schedule_->event(cursor - 1).day +
                                 (schedule_->event(cursor - 1).day_end ? 1 : 0)))
      throw std::invalid_argument("strategy settlement cursor mismatch");
  }
  if (protocol::decode_contract(snapshot.contract()) !=
          protocol::decode_contract(plan.dataset().contract()) ||
      snapshot.total() != static_cast<std::uint32_t>(plan.dataset().bars_size()) ||
      !snapshot.has_strategy())
    throw std::invalid_argument("strategy account dataset or authorization mismatch");
  const auto& g = snapshot.strategy().grant();
  if (g.grant_id() != plan.grant_id() || g.strategy_id() != config_.session_id() ||
      g.stream_id() != config_.stream_id() || g.dataset_revision() != plan.dataset().revision() ||
      g.max_quantity().units() < config_.quantity().units())
    throw std::invalid_argument("strategy replay is outside its authorization");
  if (snapshot.cursor() &&
      snapshot.timestamp_ns() !=
          plan.dataset().bars(static_cast<int>(snapshot.cursor() - 1)).timestamp_ns())
    throw std::invalid_argument("strategy account source event mismatch");
  return snapshot;
}
void Replay::probe() {
  protocol::v1::Request r;
  r.mutable_snapshot();
  static_cast<void>(request(r));
}
bool Replay::step() {
  if (session_.recovery_required())
    throw std::runtime_error("strategy journal requires recovery");
  protocol::v1::Request query;
  query.mutable_snapshot();
  auto account = request(query);
  auto processed = session_.processed();
  const auto total = static_cast<std::uint64_t>(config_.replay().dataset().bars_size());
  if (account.cursor() < processed || account.cursor() > processed + 1)
    throw std::invalid_argument("account and strategy cursors diverged");
  auto settle_completed = [&] {
    if (!account.cursor())
      return;
    const auto& event = schedule_->event(account.cursor() - 1);
    if (!event.day_end || account.replay().settled_days() == event.day + 1)
      return;
    if (!account.strategy().active())
      throw std::invalid_argument("strategy authorization was revoked before settlement");
    protocol::v1::Request r;
    r.mutable_command()->set_request_id("settle." + identity_ + "." + std::to_string(event.day));
    r.mutable_command()->mutable_replay_settle()->set_day_index(static_cast<unsigned>(event.day));
    account = request(std::move(r));
    if (account.replay().settled_days() != event.day + 1)
      throw std::runtime_error("strategy settlement acknowledgement mismatch");
  };
  auto finish = [&] {
    settle_completed();
    protocol::v1::Request r;
    r.mutable_command()->set_request_id("finish." + identity_);
    r.mutable_command()->mutable_strategy_revoke()->set_grant_id(config_.replay().grant_id());
    account = request(std::move(r));
    if (account.strategy().active())
      throw std::runtime_error("strategy grant was not revoked");
    return true;
  };
  if (processed == total && account.cursor() == total)
    return finish();
  if (!account.strategy().active())
    throw std::invalid_argument("strategy authorization was revoked");
  auto deliver = [&](std::uint64_t sequence) {
    if (!sequence)
      return;
    v1::Event event;
    event.set_stream_id(config_.stream_id());
    event.set_sequence(sequence);
    *event.mutable_bar() = config_.replay().dataset().bars(static_cast<int>(sequence - 1));
    const auto receipt = session_.apply(event);
    if (!receipt.has_intent() || sequence == total)
      return;
    protocol::v1::Request r;
    auto* command = r.mutable_command();
    command->set_request_id(receipt.intent().id());
    auto* t = command->mutable_strategy_target();
    t->set_grant_id(config_.replay().grant_id());
    t->set_strategy_id(config_.session_id());
    t->set_stream_id(config_.stream_id());
    t->set_dataset_revision(config_.replay().dataset().revision());
    t->set_sequence(sequence);
    t->set_timestamp_ns(receipt.intent().timestamp_ns());
    *t->mutable_target_quantity() = receipt.intent().target_quantity();
    account = request(std::move(r));
    if (account.cursor() != sequence || account.strategy().last_sequence() != sequence)
      throw std::runtime_error("strategy intent acknowledgement mismatch");
  };
  if (account.cursor() == processed) {
    deliver(processed); // exact idempotent replay before advancing the source clock
    settle_completed();
    protocol::v1::Request r;
    r.mutable_command()->set_request_id("advance." + identity_ + "." +
                                        std::to_string(processed + 1));
    r.mutable_command()->mutable_advance();
    account = request(std::move(r));
    if (account.cursor() != processed + 1)
      throw std::runtime_error("strategy clock acknowledgement mismatch");
  }
  deliver(processed + 1);
  settle_completed();
  if (processed + 1 == total)
    return finish();
  return false;
}
} // namespace asterion::strategy
