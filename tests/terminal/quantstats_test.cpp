#include "../apps/clients/terminal/native/analytics.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/environment.hpp>
#include <gtest/gtest.h>
#include <cmath>
#include <iomanip>
#include <sstream>
using namespace asterion;
using namespace asterion::terminal;

namespace {

// Exercise a relocated bundle as well as the checkout's prepared runtime.
const auto python =
    environment_path("ASTERION_PYTHON").value_or(std::filesystem::path{ASTERION_PYTHON_PATH});

nlohmann::json evidence()
{
    nlohmann::json source = {
        {"kind", "backtest"},
        {"id", "settlement-analysis"},
        {"task", {{"result_digest", std::string(64, 'a')}}},
        {"experiment", {{"paper", {{"deposit", "100"}}}}},
        {"result", {{"settlements", nlohmann::json::array()}}}
    };
    for (int i = 1; i <= 20; ++i) {
        std::ostringstream day;
        day << "2026-09-" << std::setfill('0') << std::setw(2) << i;
        source["result"]["settlements"].push_back(
            {{"trading_day", day.str()}, {"equity", i % 2 ? "90" : "100"}});
    }
    return source;
}

} // namespace

TEST(QuantStats, BundledRuntimeAnalyzesActualSettlementsAndInitialCapital)
{
    const auto report = quantstats_report(evidence(), python);
    EXPECT_EQ(report.at("task_id"), "settlement-analysis");
    EXPECT_EQ(report.at("library_version"), "0.0.86");
    ASSERT_EQ(report.at("returns").size(), 20);
    EXPECT_DOUBLE_EQ(report.at("returns")[0].at("value").get<double>(), -0.1);
    EXPECT_DOUBLE_EQ(report.at("returns")[1].at("value").get<double>(), 1.0 / 9);
    const auto& metrics = report.at("metrics");
    EXPECT_NEAR(metrics.at("omega").get<double>(), 10.0 / 9, 1e-12);
    EXPECT_NEAR(
        metrics.at("sortino").get<double>(),
        (1.0 / 9 - 0.1) / 2 / std::sqrt(0.005) * std::sqrt(252),
        1e-12);
    EXPECT_NEAR(metrics.at("ulcer_index").get<double>(), std::sqrt(0.005), 1e-12);
    EXPECT_LT(
        metrics.at("conditional_value_at_risk").get<double>(),
        metrics.at("value_at_risk").get<double>());
}

TEST(QuantStats, DecimalDifferencePrecedesFloatingConversionAndUndefinedMetricsStayNull)
{
    auto source = evidence();
    source["experiment"]["paper"]["deposit"] = "9007199254740993";
    for (auto& day : source["result"]["settlements"])
        day["equity"] = "9007199254740993.00000001";
    const auto report = quantstats_report(source, python);
    EXPECT_GT(report.at("returns")[0].at("value").get<double>(), 0);
    EXPECT_TRUE(report.at("metrics").at("sortino").is_null());
    EXPECT_TRUE(report.at("metrics").at("omega").is_null());
    EXPECT_DOUBLE_EQ(report.at("metrics").at("ulcer_index").get<double>(), 0);
}

TEST(QuantStats, RejectsInsufficientObservationsAndNonpositiveEquity)
{
    auto source = evidence();
    source["result"]["settlements"][5]["equity"] = "0";
    try {
        quantstats_report(source, python);
        FAIL() << "nonpositive equity was accepted";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::invalid_request);
    }
    source["result"]["settlements"] = nlohmann::json::array();
    EXPECT_THROW(quantstats_report(source, python), Error);
}
