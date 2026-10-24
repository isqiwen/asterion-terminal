#pragma once
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/backtest.hpp>
#include "data/bar_fixture.hpp"
#include <asterion/protocol/trading.hpp>
#include <stdexcept>

// A backtest account input for tests, written as the manifest a client shows.
namespace asterion::testing_support {

inline protocol::proto::PaperInput paper_input(const Json& manifest)
{
    require_fields(manifest, {"type", "deposit", "risk", "contracts"});
    if (manifest.at("type") != "historical_paper")
        throw std::invalid_argument("invalid paper input");
    protocol::proto::PaperInput result;
    result.mutable_deposit()->set_units(
        Decimal::parse(manifest.at("deposit").get<std::string>()).raw());
    *result.mutable_risk() = protocol::encode_risk(manifest.at("risk"));
    for (const auto& item : manifest.at("contracts")) {
        // Fills give nothing up unless a manifest says how many price increments.
        auto* contract = result.add_contracts();
        *contract->mutable_dataset() = protocol::encode_bar_dataset(item.at("dataset"));
        *contract->mutable_cost_schedule() =
            protocol::encode_cost_schedule(item.at("cost_schedule"));
        contract->set_slippage_ticks(item.value("slippage_ticks", 0U));
    }
    static_cast<void>(protocol::decode_input(result, protocol::DatasetView::metadata));
    return result;
}

// A moving-average strategy, as most tests trade.
inline protocol::proto::Strategy
moving_average(unsigned fast, unsigned slow, const char* quantity = "1", const char* sides = "long")
{
    return protocol::encode_strategy(
        {{"quantity", quantity},
            {"sides", sides},
            {"rule", {{"kind", "moving_average"}, {"fast", fast}, {"slow", slow}}}});
}

// One contract's bars on an account of 10000 with fixed costs, traded by a
// long-only moving average of 1 and 3.
inline backtest::proto::BacktestInput backtest_input(const data::proto::BarDataset& dataset)
{
    const auto manifest = Json{
        {"type", "historical_paper"},
        {"risk",
            {{"max_order_quantity", "100"},
                {"max_gross_quantity", "100"},
                {"max_working_orders", std::uint64_t{100}}}},
        {"deposit", "10000"},
        {"contracts",
            {{{"dataset", protocol::decode_bar_dataset(dataset)},
                {"cost_schedule",
                    test::cost_schedule(
                        {{"margin_per_lot", "100"},
                            {"open_fee", "2"},
                            {"close_today_fee", "3"},
                            {"close_yesterday_fee", "4"},
                            {"margin_rate", "0"},
                            {"open_fee_rate", "0"},
                            {"close_today_fee_rate", "0"},
                            {"close_yesterday_fee_rate", "0"}})}}}}
    };
    backtest::proto::BacktestInput result;
    *result.mutable_paper() = testing_support::paper_input(manifest);
    *result.add_strategies() = testing_support::moving_average(1, 3);
    result.set_dataset_revision(protocol::dataset_revision(result.paper()));
    return result;
}

} // namespace asterion::testing_support
