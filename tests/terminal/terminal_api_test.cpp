#include "../apps/clients/terminal/native/research_export.hpp"
#include "../apps/clients/terminal/native/evaluation_export.hpp"
#include "../apps/clients/terminal/native/portfolio_budget_review.hpp"
#include "../apps/clients/terminal/native/market_history.hpp"
#include "support/timing.hpp"
#include "data/history_fixture.hpp"
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <asterion/terminal.h>
#include <asterion/futures/futures.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <csignal>
#include <unistd.h>

using nlohmann::json;

namespace {

struct Fixture
{
    std::filesystem::path directory = std::filesystem::temp_directory_path()
        / ("asterion-terminal-test-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    Fixture()
    {
        EXPECT_TRUE((std::filesystem::create_directory(directory))) << "create temp directory";
    }

    ~Fixture()
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

// The seeded history contract with the costs these tests trade it under.
json fixture_contracts()
{
    return json::array(
        {{{"venue", "SHFE"},
            {"symbol", "rb2610"},
            {"slippage_ticks", 0},
            {"cost_schedule",
                json::array(
                    {{{"effective_from", "1970-01-01"},
                        {"source", "test fixture"},
                        {"values",
                            {{"margin_per_lot", "100"},
                                {"open_fee", "2"},
                                {"close_today_fee", "3"},
                                {"close_yesterday_fee", "4"},
                                {"margin_rate", "0"},
                                {"open_fee_rate", "0"},
                                {"close_today_fee_rate", "0"},
                                {"close_yesterday_fee_rate", "0"}}}}})}}});
}

json call(void* runtime, const json& request)
{
    std::unique_ptr<char, decltype(&asterion_terminal_free)> result(
        asterion_terminal_call(runtime, request.dump().c_str()),
        asterion_terminal_free);
    EXPECT_TRUE((result != nullptr)) << "response allocated";
    return json::parse(result.get());
}

json request(std::string method, json params = json::object())
{
    return {{"version", 1}, {"method", method}, {"params", params}};
}

json history(void* runtime, const std::vector<int>& prices, const std::string& id)
{
    auto invoke = [&](const std::string& method, json params = json::object()) {
        auto result = call(runtime, request(method, std::move(params)));
        if (result.contains("error"))
            throw std::runtime_error(result.dump());
        return result.at("result");
    };
    invoke("node.research.local.open");
    invoke("node.data.local.open");
    const auto stopped =
        invoke("node.action", {{"service", "historical-data"}, {"action", "stop"}});
    std::string directory;
    for (const auto& node : stopped.at("nodes"))
        if (node.at("id") == "local")
            for (const auto& service : node.at("health").at("services"))
                if (service.at("id") == "historical-data")
                    directory = service.at("directory").get<std::string>();
    auto selection = asterion::test::seed_history(directory, prices, id);
    // What the fixture says beside the selection itself.
    selection.erase("daily_sequence");
    selection.erase("daily_dataset_id");
    invoke("node.data.local.open");
    const auto deadline = std::chrono::steady_clock::now()
        + asterion::testing_support::bound(std::chrono::seconds(10));
    while (!invoke("runtime.snapshot").at("data").at("online").get<bool>()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("isolated data service did not become available");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return selection;
}

} // namespace

TEST(ResearchComparisonExport, PreservesColumnOrderStatesAndReasonsAndRejectsUnrelatedFields)
{
    const json task = {
        {"attempt", 1},
        {"result_digest", std::string(64, 'a')},
        {"program_artifact", std::string(64, 'b')},
        {"metadata", json::object()}
    };
    const json first = {
        {"id", "first"},
        {"kind", "factor"},
        {"task", task},
        {"experiment", {{"dataset_revision", std::string(64, 'b')}}},
        {"result", {{"samples", json::array()}}}
    };
    auto second = first;
    second["id"] = "second";
    const json matrix = {
        {"language", "en-US"},
        {"reference_id", "first"},
        {"columns",
            json::array(
                {json{{"id", "first"}, {"methodology", "comparable"}, {"reasons", json::array()}},
                    json{
                        {"id", "second"},
                        {"methodology", "independent"},
                        {"reasons", json::array({"Fixed snapshots differ"})}
                    }})},
        {"rows",
            json::array({json{
                {"label", "Pearson"},
                {"cells",
                    json::array(
                        {json{{"text", "0.0000"}, {"state", "defined"}, {"metrics", json::array()}},
                            json{
                                {"text", "Undefined"},
                                {"state", "undefined"},
                                {"metrics", json::array()}
                            }})}
            }})}
    };
    const auto content =
        asterion::terminal::comparison_evidence_document({first, second}, "research-task", matrix);
    const auto document = json::parse(content);
    EXPECT_EQ(document.at("format"), "asterion.research.comparison.evidence");
    EXPECT_EQ(document.at("matrix"), matrix);
    EXPECT_EQ(document.at("experiments").at(0).at("source").at("task_id"), "first");
    EXPECT_EQ(document.at("experiments").at(1).at("source").at("task_id"), "second");
    EXPECT_THROW(
        asterion::terminal::comparison_evidence_document({second, first}, "research-task", matrix),
        std::invalid_argument);
    auto unrelated = matrix;
    unrelated["settings"] = {{"password", "must-not-export"}};
    EXPECT_THROW(
        asterion::terminal::comparison_evidence_document(
            {first, second},
            "research-task",
            unrelated),
        asterion::Error);
    auto invalid = matrix;
    invalid["rows"][0]["cells"][0]["state"] = "unknown";
    EXPECT_THROW(
        asterion::terminal::comparison_evidence_document({first, second}, "research-task", invalid),
        std::invalid_argument);
}

TEST(ResearchEvidenceExport, ContainsOnlyResearchEvidenceAndNeverOverwritesAnExistingFile)
{
    Fixture fixture;
    const json task = {
        {"attempt", 1},
        {"result_digest", std::string(64, 'a')},
        {"program_artifact", std::string(64, 'b')},
        {"metadata", {{"name", "test"}, {"note", "notes"}, {"source_id", "original"}}}
    };
    const json source = {
        {"id", "factor-test"},
        {"kind", "factor"},
        {"task", task},
        {"experiment", {{"dataset_revision", std::string(64, 'b')}}},
        {"result", {{"samples", json::array()}}},
        {"settings", {{"password", "not-exported"}}},
        {"nodes", {{"host", "not-exported"}}}
    };
    const auto content = asterion::terminal::research_evidence_document(source, "research-task");
    const auto document = json::parse(content);
    EXPECT_EQ(document.at("format"), "asterion.research.evidence");
    EXPECT_EQ(document.at("experiment"), source.at("experiment"));
    EXPECT_EQ(document.at("result"), source.at("result"));
    EXPECT_EQ(document.at("description"), task.at("metadata"));
    EXPECT_EQ(document.at("source").at("program_artifact"), task.at("program_artifact"));
    EXPECT_TRUE(document.at("source").at("input_sha256").is_null());
    EXPECT_FALSE(document.contains("settings"));
    auto backtest = source;
    backtest["kind"] = "backtest";
    const json fixed_package = {
        {"node", "fixture-node"},
        {"service", "research-one"},
        {"domain", "research"},
        {"kind", "strategy_package"},
        {"id", "portfolio-package"},
        {"version", std::string(64, 'c')}
    };
    auto fixed_delivery = fixed_package;
    fixed_delivery["kind"] = "strategy_delivery";
    fixed_delivery["id"] = "portfolio-delivery";
    fixed_delivery["version"] = std::string(64, 'd');
    backtest["task"]["strategy_use"] = {{"package", fixed_package}, {"delivery", fixed_delivery}};
    backtest["portfolio"] = json::object();
    backtest["performance"] = json::object();
    backtest["trades"] = json::object();
    backtest["reconciliation"] = {{"state", "missing_fill_days"}, {"days", json::array()}};
    const auto backtest_document =
        json::parse(asterion::terminal::research_evidence_document(backtest, "research-task"));
    EXPECT_EQ(backtest_document.at("reconciliation"), backtest.at("reconciliation"));
    EXPECT_EQ(backtest_document.at("strategy_use"), backtest.at("task").at("strategy_use"));
    const auto portfolio_path = fixture.directory / "portfolio.evidence.json";
    const auto portfolio_content = backtest_document.dump(2) + "\n";
    asterion::terminal::save_research_evidence(portfolio_path, portfolio_content);
    std::ifstream portfolio_file(portfolio_path, std::ios::binary);
    const std::string portfolio_bytes(std::istreambuf_iterator<char>(portfolio_file), {});
    EXPECT_EQ(
        json::parse(portfolio_bytes).at("strategy_use"),
        backtest.at("task").at("strategy_use"));
    auto missing_program = source;
    missing_program["task"].erase("program_artifact");
    EXPECT_THROW(
        asterion::terminal::research_evidence_document(missing_program, "research-task"),
        std::invalid_argument);

    EXPECT_FALSE(document.contains("nodes"));
    EXPECT_EQ(content.find("not-exported"), std::string::npos);
    const auto path = fixture.directory / "evidence.json";
    const auto saved_content = [&] {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    asterion::terminal::save_research_evidence(path, content);
    EXPECT_EQ(saved_content(), content);
    EXPECT_THROW(
        asterion::terminal::save_research_evidence(path, "replacement"),
        std::invalid_argument);
    EXPECT_EQ(saved_content(), content);
    auto unsupported = source;
    unsupported["kind"] = "minute_download";
    EXPECT_THROW(
        asterion::terminal::research_evidence_document(unsupported, "research-task"),
        std::invalid_argument);
}

TEST(TerminalApi, FixedEvaluationExportKeepsOriginalValuesReasonsAndEscapesUntrustedText)
{
    const json reference = {
        {"node", "fixture-node"},
        {"service", "research-one"},
        {"domain", "research"},
        {"kind", "backtest_result"},
        {"id", "original-account"},
        {"version", std::string(64, 'b')}
    };
    json result = {
        {"first_day", "2026-09-21"},
        {"last_day", "2026-09-25"},
        {"begin_ns", "1789990000000000000"},
        {"end_ns", "1790300000000000000"},
        {"currency", "CNY"},
        {"trading_days", 5},
        {"complete_return_periods", 4},
        {"opening_is_full_period", false},
        {"short_calendar_span", true},
        {"calendar_year_days", 365.2425},
        {"opening_equity", "1000"},
        {"closing_equity", "973.5"},
        {"fees", "2"},
        {"net_profit", "-26.5"},
        {"limitations",
            {"CASH_FLOWS_NOT_EVALUATED",
                "BENCHMARK_NOT_EVALUATED",
                "RISK_AND_STRESS_NOT_EVALUATED",
                "INDEPENDENCE_NOT_ESTABLISHED",
                "HISTORICAL_AVAILABILITY_NOT_VERIFIED",
                "HUMAN_CONCLUSION_NOT_RECORDED"}}
    };
    for (const auto* key :
        {"total_return",
            "annual_return",
            "annual_volatility",
            "sharpe",
            "max_drawdown",
            "calmar",
            "winning_days"})
        result[key] = {{"state", "ANNUAL_PERIODS_NOT_CONFIGURED"}, {"value", nullptr}};
    result["total_return"] = {{"state", "AVAILABLE"}, {"value", -0.0265}};
    result["daily_drawdown"] = nullptr;
    result["exposure"] = nullptr;
    result["static_stress"] = nullptr;
    const json source = {
        {"kind", "performance_evaluation"},
        {"id", "evaluation-one"},
        {"input_sha256", std::string(64, 'a')},
        {"task",
            {{"result_digest", std::string(64, 'c')}, {"program_artifact", std::string(64, 'd')}}},
        {"experiment",
            {{"request",
                 {{"source", reference},
                     {"question", "=cmd,<script>alert(1)</script>\n\"quoted\""},
                     {"scope", "full_account"},
                     {"include_daily_drawdown", false},
                     {"include_exposure", false},
                     {"static_stress", nullptr},
                     {"method", {{"annual_periods", nullptr}, {"risk_free", "unconfigured"}}}}},
                {"source",
                    {{"input_sha256", std::string(64, 'e')},
                        {"input_bytes", "1234"},
                        {"attempt", 1},
                        {"program_artifact", std::string(64, 'f')},
                        {"settlements", 5},
                        {"first_settlement", 0},
                        {"end_settlement", 5}}}}},
        {"result", result},
        {"settings", {{"password", "must-not-export"}}},
        {"nodes", {"must-not-export"}}
    };
    const auto original = source.dump();
    const auto html = asterion::terminal::evaluation_report_document(source, "html");
    EXPECT_NE(html.find("&lt;script&gt;alert(1)&lt;/script&gt;"), std::string::npos);
    EXPECT_EQ(html.find("<script>"), std::string::npos);
    EXPECT_EQ(html.find("must-not-export"), std::string::npos);
    EXPECT_NE(html.find("ANNUAL_PERIODS_NOT_CONFIGURED"), std::string::npos);
    EXPECT_NE(html.find("INDEPENDENCE_NOT_ESTABLISHED"), std::string::npos);
    EXPECT_NE(html.find("-26.5"), std::string::npos);
    const auto csv = asterion::terminal::evaluation_report_document(source, "csv");
    EXPECT_NE(csv.find("\"amount.net_profit\",\"-26.5\",\"RECORDED\""), std::string::npos);
    EXPECT_NE(csv.find("\"'=cmd,<script>"), std::string::npos);
    EXPECT_NE(
        csv.find("\"metric.sharpe\",\"\",\"ANNUAL_PERIODS_NOT_CONFIGURED\""),
        std::string::npos);
    EXPECT_EQ(source.dump(), original);
    auto contributions = source;
    contributions["experiment"]["request"]["include_contributions"] = true;
    const auto check =
        [](const char* metric, const char* expected, const char* recorded, const char* residual) {
            return json{
                {"metric", metric},
                {"expected", expected},
                {"recorded", recorded},
                {"residual", residual}
            };
        };
    const auto member =
        [&](const char* symbol, const char* gross, const char* fee, const char* net) {
            return json{
                {"venue", "SHFE"},
                {"symbol", symbol},
                {"multiplier", "10"},
                {"gross", gross},
                {"fees", fee},
                {"net", net},
                {"final_position", check("position", "1", "1", "0")},
                {"days",
                    json::array(
                        {{{"source_index", 0},
                            {"trading_day", "2026-09-21"},
                            {"timestamp_ns", "0"},
                            {"settlement", "-97.00000001"},
                            {"opening_quantity", "0"},
                            {"quantity_change", "1"},
                            {"closing_quantity", "1"},
                            {"gross", gross},
                            {"fees", fee},
                            {"checks", json::array({check("position", "1", "1", "0")})},
                            {"fills",
                                json::array(
                                    {{{"id", std::string(symbol) + "-fill"},
                                        {"order_id", std::string(symbol) + "-order"},
                                        {"buy", true},
                                        {"quantity", "1"},
                                        {"price", "-100.00000001"},
                                        {"fee", fee}}})}}})}
            };
        };
    contributions["result"]["contributions"] = {
        {"deposit", "1000"},
        {"gross", "40"},
        {"fees", "6"},
        {"explained_net", "34"},
        {"recorded_net", "35"},
        {"residual", "1"},
        {"reconciled", false},
        {"contracts", json::array({member("A", "30", "4", "26"), member("B", "10", "2", "8")})},
        {"days",
            json::array(
                {{{"source_index", 0},
                    {"trading_day", "2026-09-21"},
                    {"timestamp_ns", "0"},
                    {"gross", "40"},
                    {"fees", "6"},
                    {"expected_balance", "1034"},
                    {"recorded_balance", "1035"},
                    {"recorded_equity", "1035"},
                    {"cumulative_residual", "1"},
                    {"checks", json::array({check("balance", "1034", "1035", "1")})}}})},
        {"final_checks", json::array({check("equity", "1034", "1035", "1")})}
    };
    const auto contribution_bytes = contributions.dump();
    for (const std::string format : {"html", "csv"}) {
        const auto document = asterion::terminal::evaluation_report_document(contributions, format);
        const auto exact = [&](const std::string& field,
                               const std::string& value,
                               const std::string& state = "RECORDED") {
            const auto needle = format == "csv"
                ? "\"" + field + "\",\"" + value + "\",\"" + state + "\""
                : "<th>" + field + "</th><td>" + value + "</td><td>" + state + "</td>";
            EXPECT_NE(document.find(needle), std::string::npos) << field;
        };
        exact("scope", "full_account");
        exact("report.result_sha256", std::string(64, 'c'));
        exact("contributions.contract.0.net", "26");
        exact("contributions.contract.1.net", "8");
        exact("contributions.recorded_net", "35");
        exact("contributions.residual", "1");
        exact("contributions.contract.1.day.0.fill.0.id", "B-fill");
        exact("contributions.contract.1.day.0.fill.0.price", "-100.00000001");
        exact("contributions.contract.1.day.0.fill.0.fee", "2");
        exact("contributions.day.0.cumulative_residual", "1");
        exact("contributions.day.0.timestamp_ns", "null", "NOT_RECORDED");
    }
    EXPECT_EQ(contributions.dump(), contribution_bytes);
    contributions["experiment"]["request"]["include_contributions"] = false;
    EXPECT_THROW(
        asterion::terminal::evaluation_report_document(contributions, "csv"),
        std::invalid_argument);
    auto with_daily = source;
    with_daily["experiment"]["request"]["include_daily_drawdown"] = true;
    auto& fixed = with_daily["result"];
    fixed["trading_days"] = 2;
    fixed["last_day"] = "2026-09-22";
    fixed["opening_equity"] = "110";
    fixed["closing_equity"] = "99";
    fixed["net_profit"] = "-11";
    fixed["total_return"] = {{"state", "AVAILABLE"}, {"value", -0.1}};
    with_daily["experiment"]["source"]["end_settlement"] = 2;
    fixed["limitations"][2] = "RISK_AND_STRESS_INCOMPLETE";
    fixed["daily_drawdown"] = {
        {"max_loss", "11"},
        {"max_loss_source_index", 1},
        {"max_fraction", {{"state", "AVAILABLE"}, {"value", 0.1}}},
        {"max_fraction_source_index", 1},
        {"points",
            json::array(
                {{{"source_index", 0},
                     {"trading_day", "2026-09-21"},
                     {"timestamp_ns", "1789990000000000000"},
                     {"equity", "110"},
                     {"peak_source_index", 0},
                     {"peak_equity", "110"},
                     {"loss", "0"},
                     {"fraction", {{"state", "AVAILABLE"}, {"value", 0}}}},
                    {{"source_index", 1},
                        {"trading_day", "2026-09-22"},
                        {"timestamp_ns", "1790076400000000000"},
                        {"equity", "99"},
                        {"peak_source_index", 0},
                        {"peak_equity", "110"},
                        {"loss", "11"},
                        {"fraction", {{"state", "AVAILABLE"}, {"value", 0.1}}}}})},
        {"episodes",
            json::array(
                {{{"peak_source_index", 0},
                    {"trough_source_index", 1},
                    {"recovery_source_index", nullptr},
                    {"observed_end_source_index", 1},
                    {"peak_to_trough_intervals", 1},
                    {"peak_to_recovery_intervals", nullptr},
                    {"observed_intervals", 1}}})}
    };
    const auto daily_html = asterion::terminal::evaluation_report_document(with_daily, "html");
    EXPECT_NE(daily_html.find("<svg"), std::string::npos);
    EXPECT_NE(daily_html.find("daily.point.1.loss"), std::string::npos);
    EXPECT_NE(daily_html.find("NOT_RECOVERED"), std::string::npos);
    const auto daily_csv = asterion::terminal::evaluation_report_document(with_daily, "csv");
    EXPECT_NE(
        daily_csv.find("\"daily.episode.0.recovery_source_index\",\"\",\"NOT_RECOVERED\""),
        std::string::npos);
    EXPECT_NE(daily_csv.find("\"daily.point.1.loss\",\"11\",\"RECORDED\""), std::string::npos);
    auto with_exposure = source;
    with_exposure["experiment"]["request"]["include_exposure"] = true;
    auto& account = with_exposure["result"];
    account["closing_equity"] = "1000";
    account["opening_equity"] = "1000";
    account["net_profit"] = "0";
    account["total_return"] = {{"state", "AVAILABLE"}, {"value", 0}};
    account["limitations"][2] = "RISK_AND_STRESS_INCOMPLETE";
    account["exposure"] = {
        {"source_index", 4},
        {"trading_day", "2026-09-25"},
        {"timestamp_ns", "1790300000000000000"},
        {"balance", "1000"},
        {"funds", {{"margin", "200"}, {"available", "800"}, {"frozen", "0"}}},
        {"covered_net_notional", "0"},
        {"covered_gross_notional", "4000"},
        {"net_to_equity", {{"state", "AVAILABLE"}, {"value", 0}}},
        {"gross_to_equity", {{"state", "AVAILABLE"}, {"value", 4}}},
        {"margin_to_equity", {{"state", "AVAILABLE"}, {"value", 0.2}}},
        {"positions",
            json::array(
                {{{"source_contract_index", 0},
                    {"venue", "SHFE"},
                    {"symbol", "rb2610"},
                    {"product", "rb"},
                    {"delivery_month", "2026-10"},
                    {"price", "100"},
                    {"multiplier", "10"},
                    {"quantity_increment", "1"},
                    {"net_quantity", "0"},
                    {"gross_quantity", "4"},
                    {"margin", "200"},
                    {"long_quantity", "2"},
                    {"short_quantity", "2"},
                    {"net_notional", "0"},
                    {"gross_notional", "4000"},
                    {"net_to_equity", {{"state", "AVAILABLE"}, {"value", 0}}},
                    {"gross_to_equity", {{"state", "AVAILABLE"}, {"value", 4}}},
                    {"margin_to_equity", {{"state", "AVAILABLE"}, {"value", 0.2}}},
                    {"covered_gross_share", {{"state", "AVAILABLE"}, {"value", 1}}}}})}
    };
    const auto exposure_csv = asterion::terminal::evaluation_report_document(with_exposure, "csv");
    EXPECT_NE(
        exposure_csv.find("\"exposure.position.0.gross_notional\",\"4000\",\"RECORDED\""),
        std::string::npos);
    EXPECT_NE(
        exposure_csv.find("\"exposure.funds.available\",\"800\",\"RECORDED\""),
        std::string::npos);
    with_exposure["experiment"]["request"]["static_stress"] = {
        {"purpose", "Explicit synthetic funding gap"},
        {"price", "80"},
        {"margin_rate", "0.4"},
        {"funds_model", "paper_balance_equity_minimum"}
    };
    account["static_stress"] = {
        {"mark_change", "-400"},
        {"equity", "600"},
        {"margin", "640"},
        {"available", "-40"},
        {"gap", "40"}
    };
    const auto stress_csv = asterion::terminal::evaluation_report_document(with_exposure, "csv");
    EXPECT_NE(
        stress_csv.find("\"stress.result.available\",\"-40\",\"RECORDED\""),
        std::string::npos);
    EXPECT_NE(
        stress_csv.find("\"stress.scenario.funds_model\",\"paper_balance_equity_minimum\""),
        std::string::npos);
    const auto stress_html = asterion::terminal::evaluation_report_document(with_exposure, "html");
    EXPECT_NE(stress_html.find("stress.result.gap</th><td>40"), std::string::npos);
    with_exposure["experiment"]["request"]["static_stress"] = nullptr;
    account["static_stress"] = nullptr;
    auto& missing = account["exposure"];
    missing["funds"] = nullptr;
    missing["covered_gross_notional"] = nullptr;
    missing["gross_to_equity"] = {{"state", "GROSS_NOT_RECORDED"}, {"value", nullptr}};
    missing["margin_to_equity"] = {{"state", "MARGIN_NOT_RECORDED"}, {"value", nullptr}};
    for (auto& row : missing["positions"]) {
        for (const auto* key :
            {"gross_quantity", "margin", "long_quantity", "short_quantity", "gross_notional"})
            row[key] = nullptr;
        for (const auto* key : {"gross_to_equity", "covered_gross_share"})
            row[key] = {{"state", "GROSS_NOT_RECORDED"}, {"value", nullptr}};
        row["margin_to_equity"] = {{"state", "MARGIN_NOT_RECORDED"}, {"value", nullptr}};
    }
    const auto missing_html = asterion::terminal::evaluation_report_document(with_exposure, "html");
    EXPECT_NE(missing_html.find("GROSS_NOT_RECORDED"), std::string::npos);
    EXPECT_NE(missing_html.find("MARGIN_NOT_RECORDED"), std::string::npos);
    EXPECT_NE(missing_html.find("exposure.funds.state</th><td>NOT_RECORDED"), std::string::npos);
    EXPECT_THROW(
        asterion::terminal::evaluation_report_document(source, "pdf"),
        std::invalid_argument);
}

TEST(TerminalApi, CreationPreservesTypedFailureAndClearsTheOutput)
{
    const char* value = std::getenv("ASTERION_ROOT");
    const std::optional<std::string> saved =
        value ? std::optional<std::string>(value) : std::nullopt;
    ASSERT_EQ(::setenv("ASTERION_ROOT", "relative", 1), 0);
    char* failure = nullptr;
    std::unique_ptr<void, decltype(&asterion_terminal_destroy)> rejected(
        asterion_terminal_create(&failure),
        asterion_terminal_destroy);
    std::unique_ptr<char, decltype(&asterion_terminal_free)> detail(
        failure,
        asterion_terminal_free);
    if (saved)
        ::setenv("ASTERION_ROOT", saved->c_str(), 1);
    else
        ::unsetenv("ASTERION_ROOT");
    ASSERT_EQ(rejected, nullptr);
    ASSERT_NE(detail, nullptr);
    const auto response = json::parse(detail.get());
    EXPECT_EQ(response.at("error").at("code"), "invalid_request");
    EXPECT_EQ(response.at("error").at("message"), "invalid Asterion root directory");
    failure = detail.get(); // The successful call must clear, not retain, this pointer.
    std::unique_ptr<void, decltype(&asterion_terminal_destroy)> runtime(
        asterion_terminal_create(&failure),
        asterion_terminal_destroy);
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(failure, nullptr);
}

TEST(TerminalApi, SnapshotRejectsMalformedQueriesBeforeReadingState)
{
    std::unique_ptr<void, decltype(&asterion_terminal_destroy)> runtime(
        asterion_terminal_create(nullptr),
        asterion_terminal_destroy);
    ASSERT_NE(runtime, nullptr);
    for (const auto& params : std::vector<json>{
             nullptr,
             json::array(),
             true,
             "invalid",
             {{"extra", true}},
             {{"since", -1}},
             {{"since", 1.5}},
             {{"since", "1"}},
             {{"since", 0}, {"extra", true}}
         }) {
        const auto response = call(runtime.get(), request("runtime.snapshot", params));
        ASSERT_TRUE(response.contains("error")) << params.dump() << ": " << response.dump();
        EXPECT_EQ(response.at("error").at("code"), "invalid_request");
    }
    EXPECT_TRUE(call(runtime.get(), request("runtime.snapshot")).contains("result"));
}

TEST(TerminalApi, Contracts)
{
    std::unique_ptr<void, decltype(&asterion_terminal_destroy)> runtime(
        asterion_terminal_create(nullptr),
        asterion_terminal_destroy);
    EXPECT_TRUE((runtime != nullptr)) << "runtime allocated";
    EXPECT_EQ(
        call(runtime.get(), request("runtime.snapshot"))["result"]["diagnostics"]["log_directory"],
        std::filesystem::path(std::getenv("ASTERION_ROOT")).string());
    EXPECT_TRUE((call(runtime.get(), request("runtime.snapshot"))["result"]["datasets"].empty()))
        << "no invented dataset";
    {
        std::unique_ptr<char, decltype(&asterion_terminal_free)> response(
            asterion_terminal_call(
                runtime.get(),
                R"({"version":1,"method":"runtime.snapshot","method":"live.close","params":{}})"),
            asterion_terminal_free);
        EXPECT_TRUE((response && json::parse(response.get())["error"]["code"] == "invalid_request"))
            << "duplicate method cannot bypass dispatch";
    }
    const auto params = history(runtime.get(), {100, 101}, "contracts");
    auto imported = call(runtime.get(), request("data.dataset.select", params));
    ASSERT_TRUE(imported.contains("result")) << imported.dump();
    ASSERT_EQ(imported["result"]["datasets"].size(), 1U);
    const auto dataset = imported["result"]["datasets"][0];
    EXPECT_EQ(dataset["count"], 2);
    EXPECT_EQ(dataset["last_close"], "101");
    EXPECT_TRUE(dataset["last_timestamp_ns"].is_string());
    auto invalid = params;
    invalid["begin_day"] = "2026-02-30";
    EXPECT_TRUE(call(runtime.get(), request("data.dataset.select", invalid)).contains("error"));
    EXPECT_EQ(
        call(runtime.get(), request("runtime.snapshot"))["result"]["datasets"],
        json::array({dataset}));
    auto wrong = request("runtime.snapshot");
    wrong["version"] = 2;
    EXPECT_TRUE((call(runtime.get(), wrong).contains("error"))) << "unsupported version rejected";
    EXPECT_TRUE((call(runtime.get(), request("missing")).contains("error")))
        << "unsupported method rejected";
    EXPECT_TRUE((call(nullptr, request("runtime.snapshot")).contains("error")))
        << "null runtime rejected without crossing ABI";
    auto extra = params;
    extra["extra"] = true;
    EXPECT_TRUE((call(runtime.get(), request("data.dataset.select", extra)).contains("error")))
        << "unknown fields rejected";
    // Selecting the same contract again replaces it; removal names the contract.
    EXPECT_EQ(
        call(runtime.get(), request("data.dataset.select", params))["result"]["datasets"].size(),
        1U);
    EXPECT_TRUE(call(
        runtime.get(),
        request("data.dataset.remove", {{"venue", "SHFE"}, {"symbol", "rb2611"}}))
            .contains("error"));
    EXPECT_TRUE(call(
        runtime.get(),
        request(
            "data.dataset.remove",
            {{"venue", "SHFE"}, {"symbol", "rb2610"}}))["result"]["datasets"]
            .empty());
    using namespace asterion;
    FuturesContract czce{
        {{"CZCE", "SR609"}, "CNY", Decimal::parse("1"), Decimal::parse("1"), Decimal::parse("10")},
        "SR",
        "2026-09"
    };
    czce.validate();
    czce.instrument.id.venue = "NYMEX";
    EXPECT_THROW(
        ([&] {
            czce.validate();
        })(),
        std::invalid_argument);
}

TEST(TerminalApi, DataServiceRestartPreservesItsDirectoryAndPublishedVersions)
{
    std::unique_ptr<void, decltype(&asterion_terminal_destroy)> runtime(
        asterion_terminal_create(nullptr),
        asterion_terminal_destroy);
    ASSERT_TRUE(runtime);
    const auto selection = history(runtime.get(), {100, 101}, "data-update");
    auto invoke = [&](const std::string& method, json params = json::object()) {
        const auto response = call(runtime.get(), request(method, std::move(params)));
        if (response.contains("error"))
            throw std::runtime_error(response.dump());
        return response.at("result");
    };
    const auto original = invoke("data.dataset.select", selection).at("datasets");
    auto service = [](const json& snapshot) {
        for (const auto& node : snapshot.at("nodes"))
            if (node.at("id") == "local")
                for (const auto& item : node.at("health").at("services"))
                    if (item.at("id") == "historical-data")
                        return item;
        throw std::runtime_error("fixture data service not found");
    };
    // The Terminal stops its child before it acknowledges.
    const auto stopped =
        service(invoke("node.action", {{"service", "historical-data"}, {"action", "stop"}}));
    EXPECT_EQ(stopped.at("state"), "stopped");
    EXPECT_FALSE(stopped.at("desired_running").get<bool>());
    EXPECT_EQ(stopped.at("kind"), "data");
    invoke("node.data.local.open");
    const auto deadline = std::chrono::steady_clock::now()
        + asterion::testing_support::bound(std::chrono::seconds(10));
    while (!invoke("runtime.snapshot").at("data").at("online").get<bool>()) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    invoke("data.dataset.clear");
    EXPECT_EQ(invoke("data.dataset.select", selection).at("datasets"), original);
}

// A dominant series the data service refuses is an error the user reads.
// Here the minute bars of both months run a day past what the daily table
// holds.
TEST(TerminalApi, ARefusedMinuteSeriesIsAnError)
{
    std::unique_ptr<void, decltype(&asterion_terminal_destroy)> runtime(
        asterion_terminal_create(nullptr),
        asterion_terminal_destroy);
    ASSERT_TRUE(runtime);
    auto invoke = [&](const std::string& method, json params = json::object()) {
        const auto response = call(runtime.get(), request(method, std::move(params)));
        if (response.contains("error"))
            throw std::runtime_error(response.dump());
        return response.at("result");
    };
    invoke("node.research.local.open");
    invoke("node.data.local.open");
    const auto stopped =
        invoke("node.action", {{"service", "historical-data"}, {"action", "stop"}});
    std::string directory;
    for (const auto& node : stopped.at("nodes"))
        if (node.at("id") == "local")
            for (const auto& service : node.at("health").at("services"))
                if (service.at("id") == "historical-data")
                    directory = service.at("directory").get<std::string>();
    std::vector<json> months;
    for (const auto* month : {"2026-10", "2026-11"})
        months.push_back(
            asterion::test::seed_history(
                directory,
                {100, 101, 102, 103},
                std::string("refused-") + month,
                {"2026-09-24", "2026-09-25"},
                {"2026-09-24"},
                "rb",
                1,
                110,
                "test.confirmed.v1",
                month));
    invoke("node.data.local.open");
    const auto deadline = std::chrono::steady_clock::now()
        + asterion::testing_support::bound(std::chrono::seconds(10));
    while (!invoke("runtime.snapshot").at("data").at("online").get<bool>()) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto refused = call(
        runtime.get(),
        request(
            "data.dataset.series",
            {{"source_dataset_ids",
                 {months[0].at("source_dataset_ids").at(0),
                     months[1].at("source_dataset_ids").at(0)}},
                {"daily", asterion::test::latest_daily(months)},
                {"begin_day", ""},
                {"end_day", ""}}));
    ASSERT_TRUE(refused.contains("error")) << refused.dump();
    EXPECT_TRUE(invoke("runtime.snapshot").at("datasets").empty());
}

TEST(TerminalApi, StatusReadsDoNotQueueBehindLongOperations)
{
    std::unique_ptr<void, decltype(&asterion_terminal_destroy)> runtime(
        asterion_terminal_create(nullptr),
        asterion_terminal_destroy);
    ASSERT_TRUE(runtime);
    const auto first = call(runtime.get(), request("runtime.snapshot"));
    ASSERT_TRUE(first.contains("result"));
    auto revision = first["result"]["revision"].get<std::uint64_t>();
    EXPECT_GT(first["result"]["refreshed_at_ms"].get<std::int64_t>(), 0);
    // The probe's own call count is published by the next background refresh,
    // so wait for the state to settle; then an unchanged revision carries no state.
    json same;
    for (int attempt = 0; attempt < 5; ++attempt) {
        same = call(runtime.get(), request("runtime.snapshot", {{"since", revision}}));
        if (same["result"].value("unchanged", false))
            break;
        revision = same["result"]["revision"].get<std::uint64_t>();
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    }
    EXPECT_EQ(same["result"]["unchanged"], true);
    EXPECT_EQ(same["result"]["revision"], revision);
    EXPECT_FALSE(same["result"].contains("datasets"));
    EXPECT_TRUE(
        call(runtime.get(), request("runtime.snapshot", {{"since", "x"}})).contains("error"));
    const auto params = history(runtime.get(), std::vector<int>(20000, 100), "concurrent");
    ASSERT_TRUE(call(runtime.get(), request("data.dataset.clear")).contains("result"));
    std::atomic<bool> done{false};
    json selected;
    std::thread slow([&] {
        // Either caller may enter first. Retry only the explicit pre-dispatch
        // admission conflict; a resolution/selection conflict remains a failure.
        const auto deadline = std::chrono::steady_clock::now()
            + asterion::testing_support::bound(std::chrono::seconds(5));
        do {
            selected = call(runtime.get(), request("data.dataset.select", params));
            if (!selected.contains("error") || selected.at("error").value("code", "") != "conflict"
                || selected.at("error").value("message", "")
                    != "another Terminal operation is in progress; retry after it completes")
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        done = true;
    });
    int concurrent_reads = 0;
    int responsive_commands = 0;
    while (!done) {
        const auto invalid = call(runtime.get(), request("runtime.snapshot", {{"extra", true}}));
        EXPECT_TRUE(invalid.contains("error"));
        if (invalid.contains("error"))
            EXPECT_EQ(invalid.at("error").at("code"), "invalid_request");
        const auto started = std::chrono::steady_clock::now();
        const auto status = call(runtime.get(), request("runtime.snapshot"));
        EXPECT_LT(
            std::chrono::steady_clock::now() - started,
            asterion::testing_support::bound(std::chrono::milliseconds(500)));
        // The import may publish between this read and the done flag; either way
        // a reader sees no dataset or the complete one, never a partial state.
        const auto& datasets = status["result"]["datasets"];
        if (datasets.empty())
            ++concurrent_reads;
        else
            EXPECT_EQ(datasets[0]["count"], 20000);
        const auto command_started = std::chrono::steady_clock::now();
        const auto inspect = call(runtime.get(), request("native.plugins.inspect"));
        EXPECT_LT(
            std::chrono::steady_clock::now() - command_started,
            asterion::testing_support::bound(std::chrono::milliseconds(500)));
        if (inspect.contains("result"))
            ++responsive_commands;
        else
            EXPECT_EQ(
                inspect["error"]["code"],
                "conflict"); // Brief selection commit may own the lock.
    }
    slow.join();
    ASSERT_TRUE(selected.contains("result")) << selected.dump();
    EXPECT_GT(concurrent_reads, 0) << "import finished before a concurrent read was observed";
    EXPECT_GT(responsive_commands, 0) << "archive reads must let other commands complete";
    const auto fresh = call(runtime.get(), request("runtime.snapshot", {{"since", revision}}));
    EXPECT_FALSE(fresh["result"].contains("unchanged"));
    EXPECT_GT(fresh["result"]["revision"].get<std::uint64_t>(), revision);
    EXPECT_EQ(fresh["result"]["datasets"][0]["count"], 20000);
}

TEST(MarketHistory, BoundsEventsAndBreaksOnGapsFailuresAndDisconnects)
{
    asterion::terminal::MarketHistory history;
    asterion::market::proto::EventBatch batch;
    batch.set_stream_id("fixture-stream");
    batch.set_latest_sequence(520);
    for (std::uint64_t i = 1; i <= 520; ++i) {
        auto* event = batch.add_events();
        event->set_sequence(i);
        auto* quote = event->mutable_quote()->mutable_quote();
        quote->mutable_instrument()->set_venue("SHFE");
        quote->mutable_instrument()->set_symbol("rb2610");
        quote->set_last("3510.00000001");
        quote->set_source_ms(1790582400000 + static_cast<std::int64_t>(i));
    }
    history.append(batch);
    auto view = history.snapshot();
    ASSERT_EQ(view.at("points").size(), 512);
    EXPECT_EQ(view.at("points").front().at("price"), "3510.00000001");
    EXPECT_EQ(view.at("points").front().at("timestamp_ns"), "1790582400009000000");
    EXPECT_EQ(history.cursor, 520);
    batch.clear_events();
    batch.set_latest_sequence(530);
    batch.set_gap(true);
    auto* event = batch.add_events();
    event->set_sequence(530);
    *event->mutable_quote()->mutable_quote() = asterion::market::proto::Quote{};
    history.append(batch);
    EXPECT_TRUE(history.snapshot().at("points").empty());
    EXPECT_TRUE(history.snapshot().at("interrupted"));
    batch.clear_events();
    batch.set_gap(false);
    batch.set_failed(true);
    history.append(batch);
    EXPECT_FALSE(history.snapshot().at("available"));
    history.interrupt();
    EXPECT_EQ(history.cursor, 0);
    EXPECT_TRUE(history.stream.empty());
    batch.set_stream_id("restarted");
    batch.set_failed(false);
    batch.set_latest_sequence(1);
    batch.add_events()->set_sequence(1);
    batch.mutable_events(0)->mutable_status()->set_phase("disconnected");
    history.append(batch);
    EXPECT_TRUE(history.snapshot().at("points").empty());
}

TEST(MarketHistory, RejectsBrokenOrderAndSkipsOutOfOrderQuotes)
{
    asterion::terminal::MarketHistory history;
    asterion::market::proto::EventBatch batch;
    batch.set_stream_id("fixture-stream");
    batch.set_latest_sequence(2);
    batch.add_events()->set_sequence(2);
    EXPECT_THROW(history.append(batch), std::invalid_argument);
    EXPECT_EQ(history.cursor, 0);
    batch.mutable_events(0)->set_sequence(1);
    auto* observation = batch.mutable_events(0)->mutable_quote();
    observation->set_out_of_order(true);
    observation->mutable_quote()->set_last("100");
    observation->mutable_quote()->set_source_ms(1000);
    history.append(batch);
    EXPECT_EQ(history.cursor, 1);
    EXPECT_TRUE(history.snapshot().at("points").empty());
    EXPECT_THROW(history.append(batch), std::invalid_argument);
}

TEST(MarketHistory, VolumeUsesPerContractDayBaselineAndResetsOnDiscontinuity)
{
    asterion::terminal::MarketHistory history;
    std::uint64_t sequence = 0;
    const auto append = [&](const char* symbol,
                            const char* day,
                            std::int64_t volume,
                            bool gap = false,
                            bool out_of_order = false) {
        asterion::market::proto::EventBatch batch;
        batch.set_stream_id("volume-fixture");
        batch.set_latest_sequence(++sequence);
        batch.set_gap(gap);
        auto* event = batch.add_events();
        event->set_sequence(sequence);
        auto* observation = event->mutable_quote();
        observation->set_out_of_order(out_of_order);
        auto* quote = observation->mutable_quote();
        quote->mutable_instrument()->set_venue("SHFE");
        quote->mutable_instrument()->set_symbol(symbol);
        quote->set_trading_day(day);
        quote->set_source_ms(1000 + static_cast<std::int64_t>(sequence));
        quote->set_last("100");
        quote->set_volume(volume);
        history.append(batch);
    };
    append("rb", "20260929", 1000);
    EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
    append("cu", "20260929", 9000);
    append("rb", "20260929", 1007);
    EXPECT_EQ(history.snapshot()["points"].back()["volume"], "7");
    append("rb", "20260929", 1007);
    EXPECT_EQ(history.snapshot()["points"].back()["volume"], "0");
    append("rb", "20260929", 3000, false, true);
    append("rb", "20260929", 1010);
    EXPECT_EQ(history.snapshot()["points"].back()["volume"], "3");
    append("rb", "20260930", 50);
    EXPECT_EQ(history.snapshot()["points"].size(), 2); // cu is retained.
    EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
    append("rb", "20260930", 40);
    EXPECT_EQ(history.snapshot()["points"].size(), 2);
    EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
    append("rb", "20260930", 45);
    EXPECT_EQ(history.snapshot()["points"].back()["volume"], "5");
    append("rb", "20260930", 100, true);
    EXPECT_EQ(history.snapshot()["points"].size(), 1);
    EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
    append("rb", "", 110);
    append("rb", "", 120);
    EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
    append("rb", "20260930", -1);
    append("rb", "20260930", 130);
    EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
}

namespace {

// A disposable root for this process, removed at exit. The services a
// Terminal starts exit with it.
class IsolatedNode
{
public:
    IsolatedNode()
        : path_(
              std::filesystem::temp_directory_path()
              / ("ast-node-test-" + asterion::unique_process_id()))
    {
        std::filesystem::create_directory(path_);
        ::setenv("ASTERION_ROOT", path_.c_str(), 1);
        ::setenv("ASTERION_TEST_NODE_ISOLATED", "1", 1);
    }

    ~IsolatedNode()
    {
        // Services still stopping may write once more.
        std::error_code ignored;
        for (int i = 0; i < 6; ++i) {
            std::filesystem::remove_all(path_, ignored);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!std::filesystem::exists(path_))
                break;
        }
    }

private:
    std::filesystem::path path_;
};

} // namespace

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    if (::testing::GTEST_FLAG(list_tests))
        return RUN_ALL_TESTS();
    // CTest runs each case with --isolated-node, so cases run in parallel, each
    // with its own root.
    std::optional<IsolatedNode> node;
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--isolated-node")
            node.emplace();
    // Direct invocation must never touch the user's own root.
    const char* directory = std::getenv("ASTERION_ROOT");
    const char* isolated = std::getenv("ASTERION_TEST_NODE_ISOLATED");
    if (!directory || !*directory || !isolated || std::string_view(isolated) != "1") {
        std::cerr
            << "Run terminal tests through CTest, with --isolated-node or " "tests/support/isolated_node.py; an isolated test root is required.\n";
        return 2;
    }
    return RUN_ALL_TESTS();
}

TEST(TerminalApi, LargeNamedDatasetsBacktest)
{
    std::unique_ptr<void, decltype(&asterion_terminal_destroy)> runtime(
        asterion_terminal_create(nullptr),
        asterion_terminal_destroy);
    auto invoke = [&](std::string method, json params = json::object()) {
        return call(runtime.get(), request(method, std::move(params)));
    };
    std::vector<int> prices(30000);
    for (std::size_t i = 0; i < prices.size(); ++i)
        prices[i] = 3000 + static_cast<int>(i % 200);
    const auto selected = invoke("data.dataset.select", history(runtime.get(), prices, "large"));
    ASSERT_TRUE(selected.contains("result")) << selected.dump().substr(0, 400);
    EXPECT_EQ(selected["result"]["datasets"][0]["count"], 30000);
    const json costs{
        {"deposit", "1000000"},
        {"contracts", fixture_contracts()},
        {"max_order_quantity", "10"},
        {"max_gross_quantity", "10"},
        {"max_working_orders", "10"}
    };
    auto backtest = costs;
    backtest.update(
        {{"id", "large-backtest"},
            {"strategies",
                json::array(
                    {{{"quantity", "1"},
                        {"sides", "long"},
                        {"rule", {{"kind", "moving_average"}, {"fast", 5}, {"slow", 20}}}}})},
            {"holdout_from", ""},
            {"walk_forward", nullptr}});
    const auto submitted = invoke("backtest.submit", backtest);
    ASSERT_TRUE(submitted.contains("result")) << submitted.dump().substr(0, 400);
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

TEST(PortfolioBudgetReview, ExplicitReviewIsTraceableAndDoesNotMutateTheSource)
{
    json source = {
        {"id", "fixed"},
        {"kind", "backtest"},
        {"input_sha256", std::string(64, 'a')},
        {"task", {{"result_digest", std::string(64, 'b')}}},
        {"portfolio", {{"gross_notional", "5130"}, {"model_margin", "1123"}}}
    };
    source["portfolio"]["settlement_path"] = {{"points", json::array()}};
    const auto original = source.dump();
    auto review = asterion::terminal::portfolio_budget_review(
        source,
        {{"gross_notional", "5000"}, {"margin", nullptr}});
    EXPECT_EQ(review.at("checks").at("gross_notional").at("headroom"), "-130");
    EXPECT_EQ(review.at("checks").at("gross_notional").at("state"), "exceeded");
    EXPECT_EQ(review.at("checks").at("margin").at("state"), "unset");
    EXPECT_EQ(review.at("source").at("input_sha256"), source.at("input_sha256"));
    EXPECT_EQ(source.dump(), original);
    EXPECT_THROW(
        asterion::terminal::portfolio_budget_review(
            source,
            {{"gross_notional", nullptr}, {"margin", nullptr}}),
        std::invalid_argument);
    EXPECT_THROW(
        asterion::terminal::portfolio_budget_review(
            source,
            {{"gross_notional", "-1"}, {"margin", nullptr}}),
        std::invalid_argument);
    EXPECT_THROW(
        asterion::terminal::portfolio_budget_review(
            source,
            {{"gross_notional", "1.000000001"}, {"margin", nullptr}}),
        std::exception);
}

TEST(
    PortfolioBudgetReview,
    EarlierRecordedExcessRemainsVisibleWhenFinalIsWithinAndMissingPointsStayUnknown)
{
    json source = {
        {"id", "fixed"},
        {"kind", "backtest"},
        {"input_sha256", std::string(64, 'a')},
        {"task", {{"result_digest", std::string(64, 'b')}}},
        {"portfolio", {{"gross_notional", "2000"}, {"model_margin", "100"}}}
    };
    const auto point = [](const char* day, json gross, json margin) {
        return json{
            {"day", day},
            {"net_notional", "0"},
            {"gross_notional", gross},
            {"model_margin", margin},
            {"balance", "10000"},
            {"equity", "10000"},
            {"available", nullptr},
            {"frozen", nullptr}
        };
    };
    source["portfolio"]["settlement_path"] = {
        {"points",
            {point("2026-09-21", "2200", "100"),
                point("2026-09-22", nullptr, nullptr),
                point("2026-09-23", "2000", "100")}}
    };
    const auto original = source.dump();
    const auto review = asterion::terminal::portfolio_budget_review(
        source,
        {{"gross_notional", "2100"}, {"margin", nullptr}});
    EXPECT_EQ(review.at("checks").at("gross_notional").at("state"), "within");
    const auto& summary = review.at("settlement_summary").at("gross_notional");
    EXPECT_EQ(summary.at("checked_points"), 2);
    EXPECT_EQ(summary.at("exceeded_points"), 1);
    EXPECT_EQ(summary.at("unknown_points"), 1);
    EXPECT_EQ(
        review.at("settlement_checks").at(0).at("checks").at("gross_notional").at("headroom"),
        "-100");
    EXPECT_EQ(
        review.at("settlement_checks").at(1).at("checks").at("gross_notional").at("state"),
        "unknown");
    EXPECT_TRUE(review.at("settlement_summary").at("margin").at("exceeded_points").is_null());
    EXPECT_EQ(source.dump(), original);
}
