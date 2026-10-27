#pragma once
#include "report_text.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <stdexcept>

namespace asterion::terminal {

// Render only verified published fields. Export never recalculates performance.
inline std::string
evaluation_report_document(const nlohmann::json& source, const std::string& format)
{
    if (source.at("kind") != "performance_evaluation" || (format != "html" && format != "csv"))
        throw std::invalid_argument("fixed evaluation report export requires HTML or CSV");
    const auto& task = source.at("task");
    const auto& input = source.at("experiment");
    const auto& request = input.at("request");
    const auto& result = source.at("result");

    struct Row
    {
        std::string field, value, state;
        bool numeric;
    };

    std::vector<Row> rows;
    const auto add = [&](std::string key, const nlohmann::json& value, bool decimal = false) {
        rows.push_back(
            {std::move(key),
                value.is_string() ? value.get<std::string>() : value.dump(),
                value.is_null() ? "NOT_RECORDED" : "RECORDED",
                decimal || value.is_number()});
    };
    add("report.id", source.at("id"));
    add("report.result_sha256", task.at("result_digest"));
    add("report.input_sha256", source.at("input_sha256"));
    add("report.evaluator_program", task.at("program_artifact"));
    add("question", request.at("question"));
    add("scope", request.at("scope"));
    add("daily.requested", request.at("include_daily_drawdown"));
    add("exposure.requested", request.at("include_exposure"));
    const auto checks = [&](const auto& rows, const std::string& prefix) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto name = prefix + "." + std::to_string(i);
            add(name + ".metric", rows.at(i).at("metric"));
            for (const auto* key : {"expected", "recorded", "residual"})
                add(name + "." + key, rows.at(i).at(key), true);
        }
    };
    const auto ledger_requested = request.value("include_ledger", false);
    if (ledger_requested != result.contains("ledger"))
        throw std::invalid_argument("evaluation result does not match its fixed input");
    if (ledger_requested) {
        const auto& ledger = result.at("ledger");
        add("ledger.requested", true);
        add("ledger.assumptions",
            "Fixed full-account single-contract daily mark bridge, including settlement effects of open positions. Not closed-trade, direction, strategy or multi-contract attribution. Differences remain unexplained, never repaired.");
        for (const auto* key : {"venue", "symbol", "reconciled"})
            add(std::string("ledger.") + key, ledger.at(key));
        for (const auto* key :
            {"multiplier",
                "deposit",
                "gross",
                "fees",
                "explained_net",
                "recorded_net",
                "residual",
                "closing_net_quantity",
                "closing_gross_quantity"})
            add(std::string("ledger.") + key, ledger.at(key), true);
        for (std::size_t i = 0; i < ledger.at("days").size(); ++i) {
            const auto& day = ledger.at("days").at(i);
            const auto prefix = "ledger.day." + std::to_string(i);
            add(prefix + ".source_index", day.at("source_index"));
            add(prefix + ".trading_day", day.at("trading_day"));
            add(prefix + ".timestamp_ns",
                day.at("timestamp_ns") == "0" ? nlohmann::json(nullptr) : day.at("timestamp_ns"),
                true);
            for (const auto* key :
                {"opening_quantity",
                    "quantity_change",
                    "closing_quantity",
                    "opening_balance",
                    "gross",
                    "fees",
                    "expected_balance",
                    "recorded_balance",
                    "recorded_equity",
                    "cumulative_residual"})
                add(prefix + "." + key, day.at(key), true);
            for (std::size_t j = 0; j < day.at("fills").size(); ++j) {
                const auto& fill = day.at("fills").at(j);
                const auto name = prefix + ".fill." + std::to_string(j);
                for (const auto* key : {"id", "order_id", "buy"})
                    add(name + "." + key, fill.at(key));
                for (const auto* key : {"quantity", "price", "fee"})
                    add(name + "." + key, fill.at(key), true);
            }
            checks(day.at("checks"), prefix + ".check");
        }
        checks(ledger.at("final_checks"), "ledger.final_check");
    }

    const bool contributions_requested = request.value("include_contributions", false);
    if (contributions_requested != result.contains("contributions"))
        throw std::invalid_argument("evaluation result does not match its fixed input");
    if (contributions_requested) {
        const auto& contribution = result.at("contributions");
        add("contributions.requested", true);
        add("contributions.assumptions",
            "Full-account original daily contract and time contributions, including open-position settlement effects. No allocated contract capital or inferred contract returns. Not strategy, direction or closed-trade attribution; residuals remain unexplained.");
        add("contributions.reconciled", contribution.at("reconciled"));
        for (const auto* key :
            {"deposit", "gross", "fees", "explained_net", "recorded_net", "residual"})
            add(std::string("contributions.") + key, contribution.at(key), true);
        for (std::size_t c = 0; c < contribution.at("contracts").size(); ++c) {
            const auto& contract = contribution.at("contracts").at(c);
            const auto prefix = "contributions.contract." + std::to_string(c);
            for (const auto* key : {"venue", "symbol"})
                add(prefix + "." + key, contract.at(key));
            for (const auto* key : {"multiplier", "gross", "fees", "net"})
                add(prefix + "." + key, contract.at(key), true);
            checks(
                nlohmann::json::array({contract.at("final_position")}),
                prefix + ".final_position");
            for (std::size_t d = 0; d < contract.at("days").size(); ++d) {
                const auto& day = contract.at("days").at(d);
                const auto name = prefix + ".day." + std::to_string(d);
                for (const auto* key : {"source_index", "trading_day"})
                    add(name + "." + key, day.at(key));
                add(name + ".timestamp_ns",
                    day.at("timestamp_ns") == "0" ? nlohmann::json(nullptr)
                                                  : day.at("timestamp_ns"),
                    true);
                for (const auto* key :
                    {"settlement",
                        "opening_quantity",
                        "quantity_change",
                        "closing_quantity",
                        "gross",
                        "fees"})
                    add(name + "." + key, day.at(key), true);
                for (std::size_t f = 0; f < day.at("fills").size(); ++f) {
                    const auto& fill = day.at("fills").at(f);
                    const auto trade = name + ".fill." + std::to_string(f);
                    for (const auto* key : {"id", "order_id", "buy"})
                        add(trade + "." + key, fill.at(key));
                    for (const auto* key : {"quantity", "price", "fee"})
                        add(trade + "." + key, fill.at(key), true);
                }
                checks(day.at("checks"), name + ".check");
            }
        }
        for (std::size_t d = 0; d < contribution.at("days").size(); ++d) {
            const auto& day = contribution.at("days").at(d);
            const auto prefix = "contributions.day." + std::to_string(d);
            for (const auto* key : {"source_index", "trading_day"})
                add(prefix + "." + key, day.at(key));
            add(prefix + ".timestamp_ns",
                day.at("timestamp_ns") == "0" ? nlohmann::json(nullptr) : day.at("timestamp_ns"),
                true);
            for (const auto* key :
                {"gross",
                    "fees",
                    "expected_balance",
                    "recorded_balance",
                    "recorded_equity",
                    "cumulative_residual"})
                add(prefix + "." + key, day.at(key), true);
            checks(day.at("checks"), prefix + ".check");
        }
        checks(contribution.at("final_checks"), "contributions.final_check");
    }

    const auto& scenario = request.at("static_stress");
    const auto& stress = result.at("static_stress");
    if (scenario.is_null() != stress.is_null())
        throw std::invalid_argument("evaluation result does not match its fixed input");
    add("stress.requested", !scenario.is_null());
    if (!scenario.is_null()) {
        add("stress.assumptions",
            "Fixed holdings, balance and frozen funds; no fills, rebalancing, fee changes or liquidation evidence. Hypothetical margin is not mark loss or a current rate.");
        for (const auto* key : {"purpose", "funds_model"})
            add(std::string("stress.scenario.") + key, scenario.at(key));
        for (const auto* key : {"price", "margin_rate"})
            add(std::string("stress.scenario.") + key, scenario.at(key), true);
        for (const auto* key : {"mark_change", "equity", "margin", "available", "gap"})
            add(std::string("stress.result.") + key, stress.at(key), true);
    }
    for (const auto* key : {"id", "version", "kind"})
        add(std::string("source.") + key, request.at("source").at(key));
    for (const auto* key :
        {"input_sha256",
            "input_bytes",
            "attempt",
            "program_artifact",
            "settlements",
            "first_settlement",
            "end_settlement"})
        add(std::string("producer.") + key, input.at("source").at(key));
    for (const auto* key : {"annual_periods", "risk_free"})
        add(std::string("method.") + key, request.at("method").at(key));
    for (const auto* key :
        {"first_day",
            "last_day",
            "begin_ns",
            "end_ns",
            "currency",
            "trading_days",
            "complete_return_periods",
            "opening_is_full_period",
            "short_calendar_span",
            "calendar_year_days"})
        add(std::string("observation.") + key, result.at(key));
    for (const auto* key : {"opening_equity", "closing_equity", "fees", "net_profit"})
        add(std::string("amount.") + key, result.at(key), true);
    for (const auto* key :
        {"total_return",
            "annual_return",
            "annual_volatility",
            "sharpe",
            "max_drawdown",
            "calmar",
            "winning_days"}) {
        const auto& metric = result.at(key);
        rows.push_back(
            {std::string("metric.") + key,
                metric.at("value").is_null() ? "" : metric.at("value").dump(),
                metric.at("state").get<std::string>(),
                true});
    }
    const auto& daily = result.at("daily_drawdown");
    if (!daily.is_null()) {
        add("daily.sampling",
            "Original settlement observations; not intraday extremes. Intervals are observation-index differences, not calendar days.");
        add("daily.max_loss", daily.at("max_loss"), true);
        add("daily.max_loss_source_index", daily.at("max_loss_source_index"));
        rows.push_back(
            {"daily.max_fraction",
                daily.at("max_fraction").at("value").is_null()
                    ? ""
                    : daily.at("max_fraction").at("value").dump(),
                daily.at("max_fraction").at("state").get<std::string>(),
                true});
        add("daily.max_fraction_source_index", daily.at("max_fraction_source_index"));
        for (const auto& point : daily.at("points")) {
            const auto prefix = "daily.point." + point.at("source_index").dump() + ".";
            for (const auto* key :
                {"source_index", "trading_day", "timestamp_ns", "peak_source_index"})
                add(prefix + key, point.at(key));
            for (const auto* key : {"equity", "peak_equity", "loss"})
                add(prefix + key, point.at(key), true);
            rows.push_back(
                {prefix + "fraction",
                    point.at("fraction").at("value").is_null()
                        ? ""
                        : point.at("fraction").at("value").dump(),
                    point.at("fraction").at("state").get<std::string>(),
                    true});
        }
        std::size_t index = 0;
        for (const auto& episode : daily.at("episodes")) {
            const auto prefix = "daily.episode." + std::to_string(index++) + ".";
            for (const auto* key :
                {"peak_source_index",
                    "trough_source_index",
                    "observed_end_source_index",
                    "peak_to_trough_intervals",
                    "observed_intervals"})
                add(prefix + key, episode.at(key));
            for (const auto* key : {"recovery_source_index", "peak_to_recovery_intervals"}) {
                const auto& value = episode.at(key);
                rows.push_back(
                    {prefix + key,
                        value.is_null() ? "" : value.dump(),
                        value.is_null() ? "NOT_RECOVERED" : "RECORDED",
                        true});
            }
        }
    } else
        add("daily.state", "NOT_REQUESTED");
    const auto& exposure = result.at("exposure");
    if (!exposure.is_null()) {
        add("exposure.coverage",
            "Only contracts listed at the original last settlement of this fixed window. Net zero does not remove gross, basis or exit risk; no cross-product margin offsets or loss limit is inferred.");
        add("exposure.units",
            "Notional and observed margin are amounts in the report currency. Net/gross to equity are multiples; concentration and margin/equity are fractions. Side lots are derived exactly from observed gross/net, not separately recorded positions.");
        for (const auto* key : {"source_index", "trading_day", "timestamp_ns"})
            add(std::string("exposure.") + key, exposure.at(key));
        for (const auto* key : {"balance", "covered_net_notional", "covered_gross_notional"})
            add(std::string("exposure.") + key, exposure.at(key), true);
        add("exposure.equity", result.at("closing_equity"), true);
        if (exposure.at("funds").is_null())
            add("exposure.funds.state", "NOT_RECORDED");
        else
            for (const auto* key : {"margin", "available", "frozen"})
                add(std::string("exposure.funds.") + key, exposure.at("funds").at(key), true);
        for (const auto* key : {"net_to_equity", "gross_to_equity", "margin_to_equity"}) {
            const auto& value = exposure.at(key);
            rows.push_back(
                {std::string("exposure.") + key,
                    value.at("value").is_null() ? "" : value.at("value").dump(),
                    value.at("state").get<std::string>(),
                    true});
        }
        for (const auto& position : exposure.at("positions")) {
            const auto prefix =
                "exposure.position." + position.at("source_contract_index").dump() + ".";
            for (const auto* key :
                {"source_contract_index", "venue", "symbol", "product", "delivery_month"})
                add(prefix + key, position.at(key));
            for (const auto* key :
                {"price",
                    "multiplier",
                    "quantity_increment",
                    "net_quantity",
                    "gross_quantity",
                    "margin",
                    "long_quantity",
                    "short_quantity",
                    "net_notional",
                    "gross_notional"})
                add(prefix + key, position.at(key), true);
            for (const auto* key :
                {"net_to_equity", "gross_to_equity", "margin_to_equity", "covered_gross_share"}) {
                const auto& value = position.at(key);
                rows.push_back(
                    {prefix + key,
                        value.at("value").is_null() ? "" : value.at("value").dump(),
                        value.at("state").get<std::string>(),
                        true});
            }
        }
    } else
        add("exposure.state", "NOT_REQUESTED");
    for (const auto& limitation : result.at("limitations"))
        add("calculation.limitation", limitation);
    add("export.scope", "Fixed performance extract; not a complete unified evaluation report.");
    add("export.limitations",
        "No account identifiers, raw market data, source files, full equity charts or executable replay bundle. No automatic trading qualification.");
    add("method.notes",
        "Calendar annual return uses original elapsed seconds and 365.2425 days. Volatility and Sharpe use explicitly selected A and sample deviation; no autocorrelation adjustment.");
    if (format == "csv") {
        std::string out = "field,value,state\r\n";
        for (const auto& row : rows)
            out += report_csv_cell(row.field) + "," + report_csv_cell(row.value, row.numeric) + ","
                + report_csv_cell(row.state) + "\r\n";
        return out;
    }
    std::string curve;
    if (!daily.is_null()) {
        const auto& points = daily.at("points");
        const auto& maximum = daily.at("max_fraction").at("value");
        const double scale =
            !maximum.is_null() && maximum.get<double>() > 0 ? maximum.get<double>() : 1;
        curve =
            "<section><h2>Daily settlement drawdown</h2><p>Original observation order, not a calendar scale or intraday risk. Undefined fractions are disconnected.</p><svg role=\"img\" aria-label=\"Original daily drawdown fractions\" viewBox=\"0 0 800 190\" style=\"width:100%;max-height:240px\">";
        std::string segment;
        const auto flush = [&] {
            if (!segment.empty()) {
                curve +=
                    "<polyline fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" points=\""
                    + segment + "\"/>";
                segment.clear();
            }
        };
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto& fraction = points[i].at("fraction").at("value");
            if (fraction.is_null()) {
                flush();
                continue;
            }
            const auto x = 30 + 740.0 * i / (points.size() > 1 ? points.size() - 1 : 1);
            const auto y = 20 + 140 * fraction.get<double>() / scale;
            segment += std::to_string(x) + "," + std::to_string(y) + " ";
        }
        flush();
        curve += "</svg></section>";
    }
    std::string out =
        "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'\"><meta name=\"viewport\" content=\"width=device-width\"><title>Fixed performance evaluation</title><style>body{font:16px system-ui;margin:2rem;max-width:90rem}table{border-collapse:collapse;width:100%}th,td{border:1px solid #aaa;padding:.5rem;text-align:left;overflow-wrap:anywhere;white-space:pre-wrap}caption{font-weight:bold;margin:1rem}</style><body><h1>Fixed performance evaluation</h1><p>Published facts and method. Missing evidence and limitations remain visible. This extract is not trading authorization.</p><table><caption>Fixed report fields and original metric states</caption><thead><tr><th>Field</th><th>Value</th><th>State</th></tr></thead><tbody>";
    if (!curve.empty())
        out.insert(out.find("<table>"), curve);
    for (const auto& row : rows)
        out += "<tr><th>" + report_html_text(row.field) + "</th><td>" + report_html_text(row.value)
            + "</td><td>" + report_html_text(row.state) + "</td></tr>";
    return out + "</tbody></table></body></html>\n";
}

} // namespace asterion::terminal
