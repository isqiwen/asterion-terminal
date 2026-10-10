#pragma once
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/trading.hpp>
#include <stdexcept>
// A backtest account input for tests, written as the manifest a client shows.
namespace asterion::testing_support {
inline protocol::v1::PaperInput paper_input(const Json& manifest) {
  require_fields(manifest, {"version", "type", "deposit", "risk", "contracts"});
  if (manifest.at("version") != 4 || manifest.at("type") != "historical_paper")
    throw std::invalid_argument("invalid paper input");
  protocol::v1::PaperInput result;
  result.mutable_deposit()->set_units(
      Decimal::parse(manifest.at("deposit").get<std::string>()).raw());
  *result.mutable_risk() = protocol::encode_risk(manifest.at("risk"));
  for (const auto& item : manifest.at("contracts")) {
    // Fills give nothing up unless a manifest says how many price increments.
    auto* contract = result.add_contracts();
    *contract->mutable_dataset() = protocol::encode_bar_dataset(item.at("dataset"));
    *contract->mutable_cost_schedule() = protocol::encode_cost_schedule(item.at("cost_schedule"));
    contract->set_slippage_ticks(item.value("slippage_ticks", 0U));
  }
  static_cast<void>(protocol::decode_input(result, protocol::DatasetView::metadata));
  return result;
}
// A moving-average strategy, as most tests trade.
inline protocol::v1::Strategy moving_average(unsigned fast, unsigned slow,
                                             const char* quantity = "1",
                                             const char* sides = "long") {
  return protocol::encode_strategy(
      {{"quantity", quantity},
       {"sides", sides},
       {"rule", {{"kind", "moving_average"}, {"fast", fast}, {"slow", slow}}}});
}
} // namespace asterion::testing_support
