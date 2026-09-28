#pragma once
#include "session.hpp"
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
  protocol::v1::Snapshot request(protocol::v1::Request value);
};
} // namespace asterion::strategy
