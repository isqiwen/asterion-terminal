#pragma once
#include "report_text.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <algorithm>
#include <vector>

namespace asterion::terminal {

// Only already verified, explicitly fixed Research metadata and original input
// conditions are rendered. No recomputation, ranking, or task dispatch.
inline std::string batch_assessment_document(
    const nlohmann::json& fixed,
    const nlohmann::json& source,
    const std::string& format)
{
    if (fixed.at("object").at("kind") != "backtest_batch_assessment"
        || (format != "html" && format != "csv"))
        throw std::invalid_argument("fixed batch assessment export requires HTML or CSV");
    const auto& snapshot = fixed.at("snapshot");
    const auto& original = snapshot.at("source");
    if (source.at("kind") != "backtest" || source.at("id") != original.at("id")
        || source.at("input_sha256") != original.at("input_sha256")
        || source.at("task").at("result_digest") != original.at("result_sha256")
        || source.at("task").at("program_artifact") != original.at("program_artifact")
        || source.at("task").at("risk_artifact") != original.at("risk_artifact"))
        throw std::invalid_argument("batch assessment original conditions identity mismatch");

    struct Row
    {
        std::string field, value, state;
        bool numeric;
    };

    std::vector<Row> rows;
    const auto add = [&](std::string key, const nlohmann::json& value, bool numeric = false) {
        rows.push_back(
            {std::move(key),
                value.is_string() ? value.get<std::string>() : value.dump(),
                value.is_null() ? "NOT_RECORDED" : "RECORDED",
                numeric || value.is_number()});
    };
    for (const auto* key : {"node", "service", "domain", "kind", "id", "version"})
        add(std::string("assessment.") + key, fixed.at("object").at(key));
    add("assessment.recorder_program", fixed.at("program_artifact"));
    add("assessment.recorded_at_ms", fixed.at("recorded_at_ms"));
    add("assessment.independence", "NOT_ESTABLISHED");
    const auto& declaration = fixed.at("declaration");
    for (const auto* key :
        {"question",
            "scope",
            "judgment",
            "reason",
            "selected_task_id",
            "selection_reason",
            "unselected_reason",
            "validation_information_used",
            "information_use",
            "previous_id",
            "revision_reason",
            "expected_snapshot"})
        add(std::string("human.") + key, declaration.at(key));
    add("selection.state",
        declaration.at("selected_task_id") == "" ? "NO_SELECTION" : "EXPLICIT_HUMAN_SELECTION");
    add("human.information_scope",
        "Retrospective declaration; declaring validation unused does not establish independence.");
    const auto& batch = snapshot.at("batch");
    const auto& definition = batch.at("definition");
    for (const auto* key : {"id", "version"})
        add(std::string("batch.") + key, definition.at("object").at(key));
    for (const auto* key :
        {"name",
            "purpose",
            "limitations",
            "run_budget",
            "time_budget_seconds",
            "source_input_sha256",
            "program_artifact",
            "risk_artifact"})
        add(std::string("batch.definition.") + key, definition.at(key));
    for (const auto* key : {"phase", "started_at_ms", "stopped_at_ms", "elapsed_ms", "reason"})
        add(std::string("batch.observation.") + key, batch.at(key));
    const auto task_rows = [&](const std::string& prefix, const nlohmann::json& task) {
        for (const auto* key :
            {"id",
                "state",
                "attempt",
                "retired",
                "error",
                "input_sha256",
                "result_sha256",
                "program_artifact",
                "risk_artifact"})
            add(prefix + "." + key, task.is_null() ? nlohmann::json(nullptr) : task.at(key));
        const auto account = task.is_null() ? nlohmann::json(nullptr) : task.at("account");
        for (const auto* key : {"currency", "first_day", "last_day", "settlement_days"})
            add(prefix + ".account." + key,
                account.is_null() ? nlohmann::json(nullptr) : account.at(key));
        for (const auto* key : {"deposit", "closing_equity", "fees", "net_profit"})
            add(prefix + ".account." + key,
                account.is_null() ? nlohmann::json(nullptr) : account.at(key),
                true);
    };
    task_rows("source", original);
    for (std::size_t i = 0; i < definition.at("members").size(); ++i) {
        const auto& planned = definition.at("members").at(i);
        const auto prefix = "member." + std::to_string(i);
        add(prefix + ".task_id", planned.at("task_id"));
        add(prefix + ".slippage_ticks", planned.at("slippage_ticks"), true);
        add(prefix + ".fee_multiplier", planned.value("fee_multiplier", std::string("1")), true);
        add(prefix + ".fee_scope",
            "Synthetic multiplier of six original commission fields; original margin and slippage unchanged for fee scenarios.");
        add(prefix + ".slippage_unit",
            "Original contract price increments; not commission or margin rate.");
        const auto receipt = std::find_if(
            batch.at("members").begin(),
            batch.at("members").end(),
            [&](const auto& row) {
                return row.at("task_id") == planned.at("task_id");
            });
        if (receipt == batch.at("members").end())
            throw std::invalid_argument("invalid batch assessment record");
        for (const auto* key : {"admission", "input_sha256", "error"})
            add(prefix + ".admission." + key, receipt->at(key));
        const auto task = std::find_if(
            snapshot.at("members").begin(),
            snapshot.at("members").end(),
            [&](const auto& row) {
                return row.at("id") == planned.at("task_id");
            });
        task_rows(
            prefix + ".observed",
            task == snapshot.at("members").end() ? nlohmann::json(nullptr) : *task);
    }
    const auto& paper = source.at("experiment").at("paper");
    for (const auto* key : {"max_order_quantity", "max_gross_quantity", "max_working_orders"})
        add(std::string("source.risk.") + key, paper.at("risk").at(key), true);
    for (std::size_t i = 0; i < paper.at("contracts").size(); ++i) {
        const auto& contract = paper.at("contracts").at(i);
        const auto prefix = "source.contract." + std::to_string(i);
        for (const auto* key : {"venue", "symbol", "product", "delivery_month", "currency"})
            add(prefix + "." + key, contract.at("dataset").at("contract").at(key));
        for (const auto* key : {"price_increment", "quantity_increment", "multiplier"})
            add(prefix + "." + key, contract.at("dataset").at("contract").at(key), true);
        add(prefix + ".dataset_revision", contract.at("dataset").at("revision"));
        add(prefix + ".original_slippage_ticks", contract.at("slippage_ticks"), true);
        for (std::size_t n = 0; n < contract.at("cost_schedule").size(); ++n) {
            const auto& cost = contract.at("cost_schedule").at(n);
            const auto name = prefix + ".cost." + std::to_string(n);
            add(name + ".effective_from", cost.at("effective_from"));
            add(name + ".source", cost.at("source"));
            for (const auto* key :
                {"margin_per_lot",
                    "margin_rate",
                    "open_fee",
                    "close_today_fee",
                    "close_yesterday_fee",
                    "open_fee_rate",
                    "close_today_fee_rate",
                    "close_yesterday_fee_rate"})
                add(name + "." + key, cost.at("values").at(key), true);
        }
    }
    add("export.scope",
        "Fixed retrospective finite-cost assessment and original conditions; not a complete unified evaluation report.");
    add("export.limitations",
        "Original full-account amounts only. All missing, failed and uncertain members are retained. No parameter-neighbourhood, dynamic liquidity, tail-risk or independent trading qualification. Includes declared narrative, failure diagnostics and stable owner identity; excludes connection configuration, credentials, raw market data and executable files.");
    if (format == "csv") {
        std::string result = "field,value,state\r\n";
        for (const auto& row : rows)
            result += report_csv_cell(row.field) + "," + report_csv_cell(row.value, row.numeric)
                + "," + report_csv_cell(row.state) + "\r\n";
        return result;
    }
    std::string result =
        "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'\"><meta name=\"viewport\" content=\"width=device-width\"><title>Fixed finite-batch assessment</title><style>body{font:16px system-ui;margin:2rem;max-width:90rem}table{border-collapse:collapse;width:100%}th,td{border:1px solid #aaa;padding:.5rem;text-align:left;overflow-wrap:anywhere;white-space:pre-wrap}</style><body><h1>Fixed finite-batch assessment</h1><p>Original declared scenarios, all observed states, missing evidence and a separate explicit human judgment. No ranking, reruns or trading permission.</p><table><thead><tr><th>Field</th><th>Value</th><th>State</th></tr></thead><tbody>";
    for (const auto& row : rows)
        result += "<tr><th>" + report_html_text(row.field) + "</th><td>"
            + report_html_text(row.value) + "</td><td>" + report_html_text(row.state)
            + "</td></tr>";
    return result + "</tbody></table></body></html>\n";
}

} // namespace asterion::terminal
