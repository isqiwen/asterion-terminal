#include "account_policy.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/trading.hpp>

namespace asterion::trading {
AccountPolicy::AccountPolicy(const Json& definition, std::string id, risk_providers::Module module)
    : revision(std::move(id)), limits(decode_order_limits(definition.at("risk"))),
      max_price_deviation(Decimal::parse(definition.at("max_price_deviation").get<std::string>())),
      algorithm(std::move(module)) {
  validate_id(revision);
  const auto input = protocol::encode_live_policy(definition);
  for (const auto& contract : input.contracts()) {
    FuturesContract value{protocol::instrument(contract), contract.product(),
                          contract.delivery_month()};
    contracts.push_back(std::move(value));
  }
  auto instance = algorithm.create(limits);
  instance->start();
  risk = std::move(instance);
}
Json AccountPolicy::definition() const {
  Json values = Json::array();
  for (const auto& contract : contracts) {
    const auto& terms = contract.instrument;
    values.push_back({{"venue", terms.id.venue},
                      {"symbol", terms.id.symbol},
                      {"currency", terms.quote_currency},
                      {"price_increment", terms.price_increment.str()},
                      {"quantity_increment", terms.quantity_increment.str()},
                      {"multiplier", terms.multiplier.str()},
                      {"product", contract.product},
                      {"delivery_month", contract.delivery_month}});
  }
  return {{"risk", encode_order_limits(limits)},
          {"max_price_deviation", max_price_deviation.str()},
          {"contracts", std::move(values)}};
}
const FuturesContract* AccountPolicy::find(const InstrumentId& id) const {
  for (const auto& contract : contracts)
    if (contract.instrument.id == id)
      return &contract;
  return nullptr;
}
void AccountPolicy::preserve_exposure(const AccountPolicy& previous, const InstrumentId& id) const {
  const auto* next = find(id);
  if (!next)
    throw Error(ErrorCode::conflict,
                "policy must retain contracts with positions or working orders");
  const auto* old = previous.find(id);
  if (old && (old->instrument.quote_currency != next->instrument.quote_currency ||
              old->instrument.price_increment != next->instrument.price_increment ||
              old->instrument.quantity_increment != next->instrument.quantity_increment ||
              old->instrument.multiplier != next->instrument.multiplier ||
              old->product != next->product || old->delivery_month != next->delivery_month))
    throw Error(ErrorCode::conflict, "policy cannot change contract units while exposure exists");
}
void AccountPolicy::capture(const std::filesystem::path& plugins) const {
  const auto directory = plugins / algorithm.artifact();
  create_directories_durably(directory);
  const auto file = risk_providers::Module::filename(directory);
  if (std::filesystem::exists(file)) {
    if (std::filesystem::is_symlink(file) || !std::filesystem::is_regular_file(file) ||
        sha256_file(file) != algorithm.artifact())
      throw std::invalid_argument("risk plugin artifact does not match its owner");
  } else {
    algorithm.capture(directory);
  }
}
} // namespace asterion::trading
