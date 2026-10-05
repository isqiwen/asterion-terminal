#pragma once
#include "risk_module.hpp"
#include <asterion/domain/futures.hpp>

namespace asterion::trading {
// One immutable policy owns its algorithm and typed parameters. It owns no
// positions or order reservations: those remain facts of the account.
struct AccountPolicy {
  std::string revision;
  OrderLimitsConfig limits;
  Decimal max_price_deviation;
  std::vector<FuturesContract> contracts;
  risk_providers::Module algorithm;
  std::shared_ptr<RiskPort> risk;

  AccountPolicy(const Json& definition, std::string revision, risk_providers::Module algorithm);
  Json definition() const;
  const FuturesContract* find(const InstrumentId& id) const;
  void preserve_exposure(const AccountPolicy& previous, const InstrumentId& id) const;
  // Digest directories retain every historical policy's exact algorithm bytes.
  void capture(const std::filesystem::path& plugins) const;
};
} // namespace asterion::trading
