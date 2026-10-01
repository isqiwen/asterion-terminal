#pragma once
#include "ctp_trader.hpp"
#include "risk_module.hpp"
#include "sqlite_journal.hpp"
#include <map>
#include <optional>
namespace asterion::trading {
// One live CTP account session in a dedicated directory. The broker owns the
// ledger; the session owns the execution chain in front of it: the owner's
// authorization, the contract allowlist and exchange units, the pinned
// pre-trade risk plugin, and a durable record of every order before it is
// sent. Orders are never resent: after a crash or disconnect the recorded
// broker keys only attribute broker reports.
class LiveSession {
public:
  // create_manifest absent means recover an existing session without rewriting it.
  LiveSession(std::filesystem::path directory, const std::filesystem::path& ctp_library,
              const Json& create_manifest = nullptr);
  ~LiveSession();
  // Credentials live in the trader's memory for this connection only.
  void connect(std::string password, std::string auth_code);
  void disconnect();
  // Asks the broker for the account's rates on every allowed contract.
  void query_costs();
  void execute(const Json& command);
  Json snapshot() const;
  bool recovery_required() const noexcept { return failed_; }

private:
  struct Intent {
    std::string broker_key, trading_day;
    InstrumentId instrument;
    Offset offset = Offset::open;
    Decimal quantity;
  };
  void append(const Json& record);
  const Instrument& allowed(const InstrumentId& id) const;
  void submit(const Json& command);
  void check_price(const LimitOrder& order, const std::optional<BrokerQuote>& quote) const;
  std::vector<std::pair<std::string, const Intent*>> unconfirmed(const BrokerSnapshot& state) const;
  SqliteJournal journal_;
  std::optional<risk_providers::Module> risk_module_;
  std::shared_ptr<RiskPort> risk_;
  std::unique_ptr<ctp::Trader> trader_;
  Json manifest_;
  std::vector<Instrument> contracts_;
  // Request ID -> command, for idempotent retries of recorded commands.
  std::map<std::string, Json> commands_;
  // Order ID -> what was recorded before sending it.
  std::map<std::string, Intent> intents_;
  // Valid for this connection and its trading day; never restored.
  Json authorization_ = nullptr;
  std::uint64_t authorization_generation_ = 0;
  bool authorized(const BrokerSnapshot& state) const;
  bool failed_ = false;
};
} // namespace asterion::trading
