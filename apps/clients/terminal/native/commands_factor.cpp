#include "application_impl.hpp"
#include <asterion/protocol/factor.hpp>

namespace asterion::terminal {

void Application::Impl::register_factor_commands()
{
    command("factor.reuse", [this](const json& p) -> PolledTask<Response> {
        auto required = p;
        required.erase("lookbacks");
        fields(
            required,
            {"id", "source_id", "source_result_digest", "experiment", "mode", "snapshot"});
        validate_id(text(p, "id"));
        validate_id(text(p, "source_id"));
        if (text(p, "id") == text(p, "source_id"))
            throw std::invalid_argument("experiment reuse requires a new task id");
        const auto mode = text(p, "mode");
        if (!p.at("snapshot").is_string())
            throw std::invalid_argument("explicit experiment data choice required");
        const auto selected = p.at("snapshot").get<std::string>();
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
        if (source.at("kind") != "factor"
            || source.at("task").at("result_digest") != p.at("source_result_digest")
            || source.at("experiment") != p.at("experiment"))
            throw std::invalid_argument(
                "experiment evidence changed; reopen its result before reuse");
        const auto request = protocol::reuse_daily_factor(
            source.at("experiment"),
            selected,
            p.value("lookbacks", json(nullptr)));
        task::proto::TaskDerivation derivation;
        derivation.set_source_id(text(p, "source_id"));
        derivation.set_source_result_digest(text(p, "source_result_digest"));
        derivation.set_data_choice(mode);
        derivation.set_snapshot(selected);
        co_await PollFuture{client->submit(
            text(p, "id"),
            request,
            mode == "snapshot" ? research_data_owner() : data::proto::DataOwner{},
            derivation)};
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/research service selection changed during operation; " "inspect the original services before retrying");
        co_return snapshot();
    });
    command("factor.submit", [this](const json& p) -> PolledTask<Response> {
        auto required = p;
        required.erase("liquidity");
        required.erase("retain_observations");
        required.erase("variable_definition");
        fields(required, {"id", "series", "factor", "lookbacks", "horizon", "evaluation"});
        validate_id(text(p, "id"));
        // A factor reads the bars this window has selected, or instruments of the
        // market-wide daily table over the ranges given for them.
        // Each selected contract is a series, and so is each dominant series, which
        // sends every month it was resolved from, or the product whose months Data
        // finds in the daily table.
        const auto& series = p.at("series");
        auto sources = json::array();
        if (series.is_object() && series.value("kind", "") == "bars") {
            fields(series, {"kind"});
            for (const auto& dataset : selected())
                if (!series_of(dataset.dataset.contract()))
                    sources.push_back(
                        {{"bars", protocol::decode_bar_dataset_request(dataset.request)}});
            for (const auto& item : dataset_series) {
                if (item.product) {
                    sources.push_back(
                        {{"dominant",
                            {{"product", protocol::decode_product_bars_request(*item.product)}}}});
                    continue;
                }
                auto months = json::array();
                for (const auto& month : item.months)
                    months.push_back(protocol::decode_bar_dataset_request(month));
                sources.push_back({{"dominant", std::move(months)}});
            }
        } else if (series.is_object() && series.value("kind", "") == "tables") {
            fields(
                series,
                {"kind", "snapshot", "daily_source", "calendar_source", "end_day", "ranges"});
            // Each range is asked of Data by itself: instruments of an exchange that
            // began inside the days asked for are read from the day it began.
            const auto& ranges = series.at("ranges");
            if (!ranges.is_array() || ranges.empty())
                throw std::invalid_argument("invalid factor series");
            for (const auto& range : ranges) {
                fields(range, {"begin_day", "instruments"});
                sources.push_back(
                    {{"tables",
                        {{"snapshot", series.at("snapshot")},
                            {"daily_source", series.at("daily_source")},
                            {"calendar_source", series.at("calendar_source")},
                            {"begin_day", range.at("begin_day")},
                            {"end_day", series.at("end_day")},
                            {"instruments", range.at("instruments")}}}});
            }
        } else
            throw std::invalid_argument("invalid factor series");
        json parameters{
            {"series", std::move(sources)},
            {"factor", p.at("factor")},
            {"lookbacks", p.at("lookbacks")},
            {"horizon", p.at("horizon")},
            {"evaluation", p.at("evaluation")}
        };
        if (p.contains("liquidity"))
            parameters["liquidity"] = p.at("liquidity");
        if (p.contains("retain_observations"))
            parameters["retain_observations"] = p.at("retain_observations");
        if (p.contains("variable_definition"))
            parameters["variable_definition"] = p.at("variable_definition");
        const auto request = protocol::encode_factor_request(parameters);
        if (!research_client)
            throw std::invalid_argument("research service is not connected");
        const auto client = research_client;
        const auto generation = data_task_generation;
        (co_await PollFuture{client->submit(text(p, "id"), request, research_data_owner())});
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/research service selection changed during operation; inspect the " "original services before retrying");
        co_return snapshot();
    });
}

} // namespace asterion::terminal
