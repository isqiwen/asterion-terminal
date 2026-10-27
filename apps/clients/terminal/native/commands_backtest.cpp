#include "application_impl.hpp"
#include "portfolio_budget_review.hpp"
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/research_strategy.hpp>

namespace asterion::terminal {

void Application::Impl::register_backtest_commands()
{
    command("backtest.margins.preview", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        if (!data_client || selected().size() != 1 || selected()[0].dataset.interval_minutes() != 0)
            throw std::invalid_argument(
                "settlement margin requires a single-contract full-sample daily backtest");
        const auto generation = data_task_generation;
        const auto dataset = selected()[0].dataset;
        data::proto::HistoricalMarginQuery query;
        query.set_contract(protocol::history_identity(dataset.contract()).key());
        for (const auto& day : dataset.days())
            query.add_trading_days(day.trading_day());
        const auto reader = data_client;
        auto margins = co_await PollFuture{reader->historical_margins(query)};
        if (generation != data_task_generation || selected().size() != 1
            || selected()[0].dataset.revision() != dataset.revision())
            throw Error(ErrorCode::conflict, "historical margin selection changed; preview again");
        auto reply = snapshot();
        reply.values["historical_margins"] = std::move(margins);
        co_return reply;
    });
    command("backtest.budget.review", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "input_sha256", "result_digest", "limits"});
        validate_id(text(p, "id"));
        if (!research_client)
            throw std::invalid_argument("research service is not connected");
        const auto client = research_client;
        const auto generation = data_task_generation;
        auto source = co_await PollFuture{client->result(text(p, "id"))};
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/research service selection changed during operation; " "inspect the original services before retrying");
        if (source.value("input_sha256", json(nullptr)) != p.at("input_sha256")
            || source.at("task").at("result_digest") != p.at("result_digest"))
            throw Error(
                ErrorCode::conflict,
                "portfolio budget evidence changed; reload result before review");
        auto review = portfolio_budget_review(source, p.at("limits"));
        review["source"]["task_instance"] = client->owner_view().connection.at("service");
        auto reply = snapshot();
        reply.values["portfolio_budget_review"] = std::move(review);
        co_return reply;
    });
    command("backtest.reuse", [this](const json& p) -> PolledTask<Response> {
        auto required = p;
        required.erase("slippage_ticks");
        required.erase("fee_multiplier");
        required.erase("strategy_use");
        fields(
            required,
            {"id", "source_id", "source_result_digest", "source_input_sha256", "mode", "snapshot"});
        validate_id(text(p, "id"));
        validate_id(text(p, "source_id"));
        if (text(p, "id") == text(p, "source_id"))
            throw std::invalid_argument("experiment reuse requires a new task id");
        if (!p.at("snapshot").is_string())
            throw std::invalid_argument("explicit experiment data choice required");
        const auto mode = text(p, "mode"), selected = p.at("snapshot").get<std::string>();
        if ((mode != "fixed" && mode != "snapshot")
            || (mode == "fixed" ? !selected.empty() : selected.empty()))
            throw std::invalid_argument("explicit experiment data choice required");
        if (!research_client)
            throw std::invalid_argument("research service is not connected");
        const auto client = research_client;
        const auto generation = data_task_generation;
        const auto source = co_await PollFuture{client->result(text(p, "source_id"))};
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/research service selection changed during operation; " "inspect the original services before retrying");
        if (source.at("kind") != "backtest"
            || source.at("task").at("result_digest") != p.at("source_result_digest")
            || source.at("input_sha256") != p.at("source_input_sha256"))
            throw std::invalid_argument(
                "experiment evidence changed; reopen its result before reuse");
        backtest::proto::BacktestReuse reuse;
        reuse.set_source_id(text(p, "source_id"));
        reuse.set_source_result_digest(text(p, "source_result_digest"));
        reuse.set_source_input_sha256(text(p, "source_input_sha256"));
        reuse.set_choice(
            mode == "fixed" ? backtest::proto::BacktestReuse::FIXED
                            : backtest::proto::BacktestReuse::NEW_SNAPSHOT);
        reuse.set_snapshot(selected);
        if (p.contains("fee_multiplier")) {
            if (p.contains("slippage_ticks"))
                throw std::invalid_argument("fee and slippage scenarios require separate requests");
            reuse.mutable_fee_multiplier()->set_units(
                Decimal::parse(text(p, "fee_multiplier")).raw());
        }
        if (p.contains("slippage_ticks")) {
            const auto& ticks = p.at("slippage_ticks");
            if (!ticks.is_number_integer() || ticks < 0 || ticks > 100)
                throw std::invalid_argument("slippage is 0 to 100 price increments");
            reuse.set_slippage_ticks(ticks.get<unsigned>());
        }
        co_await PollFuture{client->reuse_backtest(
            text(p, "id"),
            reuse,
            mode == "snapshot" ? research_data_owner() : data::proto::DataOwner{},
            p.contains("strategy_use") ? protocol::encode_strategy_use(p.at("strategy_use"))
                                       : research::proto::StrategyUse{})};
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/research service selection changed during operation; " "inspect the original services before retrying");
        co_return snapshot();
    });
    command("backtest.submit", [this](const json& p) -> PolledTask<Response> {
        auto required = p;
        required.erase("settlement_margins");
        required.erase("margin_snapshot");
        required.erase("strategy_use");
        required.erase("performance_assumptions");
        fields_with_risk(
            required,
            {"id", "strategies", "holdout_from", "walk_forward", "deposit", "contracts"});
        if (!research_client)
            throw std::invalid_argument("research service is not connected");
        const auto generation = data_task_generation;
        // What each selected contract trades at: its cost schedule and slippage.
        const auto costs = selection_costs(p.at("contracts"));
        json contracts = json::array(), series = json::array();
        const auto traded = [](const data::proto::BarDatasetRequest& data, json cost) {
            cost["data"] = protocol::decode_bar_dataset_request(data);
            return cost;
        };
        for (std::size_t i = 0; i < selected().size(); ++i)
            if (!series_of(selected()[i].dataset.contract()))
                contracts.push_back(traded(selected()[i].request, costs[i]));
        // A series sends every month it was resolved from, so the service works
        // out the same schedule. A month that never trades takes the costs of
        // one that does; they are not used.
        for (const auto& item : dataset_series) {
            // A product of the daily table is sent as the product: the service finds
            // its months again, and those that trade take the series' costs.
            if (item.product) {
                std::optional<json> cost;
                for (std::size_t i = 0; i < selected().size() && !cost; ++i)
                    if (series_of(selected()[i].dataset.contract())
                        == static_cast<std::size_t>(&item - dataset_series.data()))
                        cost = costs[i];
                auto traded = cost.value();
                traded["product"] = protocol::decode_product_bars_request(*item.product);
                series.push_back(std::move(traded));
                continue;
            }
            json members = json::array();
            std::optional<json> any;
            std::vector<std::optional<json>> known;
            for (const auto& month : item.months) {
                std::optional<json> cost;
                for (std::size_t i = 0; i < selected().size(); ++i)
                    if (protocol::history_identity(selected()[i].dataset.contract()).key()
                        == month.contract())
                        cost = costs[i];
                if (cost && !any)
                    any = cost;
                known.push_back(std::move(cost));
            }
            for (std::size_t m = 0; m < item.months.size(); ++m) {
                members.push_back(contracts.size());
                contracts.push_back(traded(item.months[m], known[m] ? *known[m] : any.value()));
            }
            series.push_back(std::move(members));
        }
        auto request = protocol::encode_backtest_request(
            {{"contracts", std::move(contracts)},
                {"deposit", text(p, "deposit")},
                {"risk", risk_parameters(p)},
                {"strategies", p.at("strategies")},
                {"holdout_from", p.at("holdout_from")},
                {"walk_forward", p.at("walk_forward")},
                {"performance_assumptions", p.value("performance_assumptions", json(nullptr))},
                {"settlement_margins", p.value("settlement_margins", json::array())},
                {"series", std::move(series)}});
        if (p.contains("margin_snapshot")) {
            const auto value = text(p, "margin_snapshot");
            if (value.size() != 64
                || value.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw std::invalid_argument("historical margin snapshot is unavailable");
            request.set_margin_snapshot(value);
        }
        (co_await PollFuture{research_client->submit(
            text(p, "id"),
            request,
            research_data_owner(),
            p.contains("strategy_use") ? protocol::encode_strategy_use(p.at("strategy_use"))
                                       : research::proto::StrategyUse{})});
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/research service selection changed during operation; inspect the " "original services before retrying");
        co_return snapshot();
    });
}

} // namespace asterion::terminal
