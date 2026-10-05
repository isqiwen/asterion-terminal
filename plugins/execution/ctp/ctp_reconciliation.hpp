#pragma once
#include <asterion/domain/broker_execution.hpp>
#include "ctp_order_identity.hpp"
#include <ThostFtdcUserApiStruct.h>
#include <array>
#include <map>
#include <set>
namespace asterion::ctp {
// CTP report projection and identity indexes. The account owner applies these
// through Trader; SDK and callback threads never mutate them. This class never calls the SDK,
// schedules a request, writes files or changes connection generations.
class ReportReconciler final {
public:
  explicit ReportReconciler(BrokerSnapshot& state) : state_(state) {}
  ReportReconciler(const ReportReconciler&) = delete;
  ReportReconciler& operator=(const ReportReconciler&) = delete;
  static std::string key(int front, int session, const std::string& ref);
  void reset(KnownOrders known);
  // Fresh login discards cached reports but retains durable order identities.
  void clear_reports();
  void begin_day(std::string day);
  void abandon_positions();
  // True means an additional fill / previously unseen trade requires refresh.
  bool apply(const CThostFtdcOrderField& report);
  bool apply(const CThostFtdcTradeField& report);
  void apply(const CThostFtdcInvestorPositionField& row);
  // Returns whether exposure changed while this position query was in flight.
  bool finish_positions(std::uint64_t query_revision);
  void remember(OrderIdentity identity, KnownOrder order);
  bool contains(const std::string& order_id) const;
  void reject(const BrokerOrder& request, int code);
  void action_failed(const std::string& broker_key, int code);
  // Borrowed references/pointers are valid only under the caller's mutex.
  const BrokerOrder* find(const std::string& broker_key) const;
  const BrokerOrder* find_order(const std::string& order_id) const;
  using RawOrderId = std::array<char, sizeof(TThostFtdcOrderSysIDType)>;
  const RawOrderId& raw_exchange_id(const std::string& broker_key) const;

private:
  BrokerOrder& order_for(const std::string& broker_key);
  bool fills_reported() const;
  BrokerSnapshot& state_;
  KnownOrders known_;
  std::map<std::string, OrderIdentity> keys_;
  std::map<std::string, std::string> exchange_;
  std::map<std::string, std::size_t> order_at_;
  std::map<std::string, RawOrderId> raw_order_ids_;
  std::set<std::string> trade_ids_;
  std::map<std::string, Decimal> traded_quantities_;
  std::map<std::pair<InstrumentId, Side>, BrokerPosition> positions_in_progress_;
};
} // namespace asterion::ctp
