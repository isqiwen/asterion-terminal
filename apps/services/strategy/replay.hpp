#pragma once
#include "session.hpp"
#include "paper_execution.hpp"
#include <functional>
namespace asterion {
class PaperReplaySchedule;
}
namespace asterion::strategy {
using TradingCall = std::function<protocol::v1::Snapshot(protocol::v1::Request)>;
// One step at a time under the host's session mutex. The immutable plan and
// strategy journal, plus the trading journal, determine recovery; no second
// ledger.
class Replay {
public:
  explicit Replay(Session& session, TradingCall transport = {});
  void probe();
  // true means the replay has reached its end and its grant has been revoked.
  bool step();

private:
  Session& session_;
  v1::Config config_;
  std::string identity_;
  TradingCall call_;
  std::shared_ptr<const PaperReplaySchedule> schedule_;
  // Portfolio replay order shared with the paper engine.
  std::vector<PaperExecution::Event> order_;
  std::string dataset_revision_;
  protocol::v1::Snapshot request(protocol::v1::Request value);
  // The replay event's source bar, in portfolio order.
  const protocol::v1::Bar& bar(std::size_t index) const;
};
} // namespace asterion::strategy
