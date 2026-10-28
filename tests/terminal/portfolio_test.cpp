#include "../apps/clients/terminal/native/analytics.hpp"
#include <asterion/foundation/error.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <iomanip>
#include <sstream>

using namespace asterion;
using namespace asterion::terminal;

namespace {

std::string day(int offset)
{
    const auto date = std::chrono::year_month_day(
        std::chrono::sys_days(std::chrono::year(2026) / 7 / 1) + std::chrono::days(offset));
    std::ostringstream text;
    text << int(date.year()) << '-' << std::setfill('0') << std::setw(2) << unsigned(date.month())
         << '-' << std::setw(2) << unsigned(date.day());
    return text.str();
}

nlohmann::json evidence(int asset)
{
    nlohmann::json source = {
        {"kind", "backtest"},
        {"id", "portfolio-" + std::to_string(asset)},
        {"task", {{"result_digest", std::string(64, 'a' + asset)}}},
        {"experiment", {{"paper", {{"deposit", "100000"}}}}},
        {"result", {{"settlements", nlohmann::json::array()}}}
    };
    int equity = 100000;
    for (int i = 0; i < 60; ++i) {
        // Different deterministic settlement P&L; asset 1 has larger variation.
        equity += asset ? ((i * 7) % 11 - 5) * 500 : ((i * 3) % 7 - 3) * 100;
        source["result"]["settlements"].push_back(
            {{"trading_day", day(i)}, {"equity", std::to_string(equity)}});
    }
    return source;
}

} // namespace

TEST(Portfolio, BundledRuntimeFitsTrainingOnlyAndPredictsBothValidationPortfolios)
{
    std::vector<nlohmann::json> sources{evidence(0), evidence(1)};
    const auto report = portfolio_report(sources, day(29), ASTERION_PYTHON_PATH);
    EXPECT_EQ(report.at("polars_version"), "2.0.0");
    EXPECT_EQ(report.at("skfolio_version"), "1.9.0");
    EXPECT_EQ(report.at("training_observations"), 30);
    ASSERT_EQ(report.at("validation").size(), 30);
    const double w0 = report.at("weights")[0].at("weight");
    const double w1 = report.at("weights")[1].at("weight");
    EXPECT_GT(w0, w1);
    EXPECT_NEAR(w0 + w1, 1, 1e-6);
    double optimized_equity = 1, equal_equity = 1;
    for (int i = 30; i < 60; ++i) {
        double returns[2];
        for (int j = 0; j < 2; ++j) {
            const auto& rows = sources[j].at("result").at("settlements");
            const double previous = std::stod(rows[i - 1].at("equity").get<std::string>());
            returns[j] = (std::stod(rows[i].at("equity").get<std::string>()) - previous) / previous;
        }
        const double optimized = w0 * returns[0] + w1 * returns[1];
        const double equal = (returns[0] + returns[1]) / 2;
        const auto& row = report.at("validation")[i - 30];
        EXPECT_EQ(row.at("trading_day"), day(i));
        EXPECT_NEAR(row.at("optimized_return").get<double>(), optimized, 1e-12);
        EXPECT_NEAR(row.at("equal_weight_return").get<double>(), equal, 1e-12);
        optimized_equity *= 1 + optimized;
        equal_equity *= 1 + equal;
    }
    EXPECT_NEAR(report.at("optimized_total_return").get<double>(), optimized_equity - 1, 1e-12);
    EXPECT_NEAR(report.at("equal_weight_total_return").get<double>(), equal_equity - 1, 1e-12);
    // Change only validation evidence; fitted weights must remain identical.
    sources[0]["task"]["result_digest"] = std::string(64, 'c');
    for (int i = 30; i < 60; ++i)
        sources[0]["result"]["settlements"][i]["equity"] = std::to_string(110000 + 100 * i);
    const auto changed = portfolio_report(sources, day(29), ASTERION_PYTHON_PATH);
    EXPECT_DOUBLE_EQ(changed.at("weights")[0].at("weight").get<double>(), w0);
    EXPECT_DOUBLE_EQ(changed.at("weights")[1].at("weight").get<double>(), w1);
    EXPECT_EQ(changed.at("weights")[0].at("result_sha256"), std::string(64, 'c'));
    EXPECT_NE(changed.at("optimized_total_return"), report.at("optimized_total_return"));
}

TEST(Portfolio, PolarsExcludesMismatchedIntervalsInsteadOfFillingMissingDates)
{
    std::vector<nlohmann::json> sources{evidence(0), evidence(1)};
    auto& rows = sources[1]["result"]["settlements"];
    rows.erase(40);
    rows.erase(10);
    const auto report = portfolio_report(sources, day(29), ASTERION_PYTHON_PATH);
    EXPECT_EQ(report.at("training_observations"), 28);
    EXPECT_EQ(report.at("validation").size(), 28);
    EXPECT_EQ(report.at("weights")[0].at("source_observations"), 60);
    EXPECT_EQ(report.at("weights")[1].at("source_observations"), 58);
    for (const auto& row : report.at("validation")) {
        EXPECT_NE(row.at("trading_day"), day(40));
        EXPECT_NE(row.at("trading_day"), day(41));
    }
}

TEST(Portfolio, RejectsShortValidationAndUninformativeTraining)
{
    std::vector<nlohmann::json> sources{evidence(0), evidence(1)};
    try {
        portfolio_report(sources, day(40), ASTERION_PYTHON_PATH);
        FAIL() << "short validation accepted";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::invalid_request);
        EXPECT_STREQ(
            error.what(),
            "Portfolio analysis requires 20 common training and 20 validation observations");
    }
    for (auto& row : sources[0]["result"]["settlements"])
        row["equity"] = "100000";
    try {
        portfolio_report(sources, day(29), ASTERION_PYTHON_PATH);
        FAIL() << "constant training accepted";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::invalid_request);
        EXPECT_STREQ(error.what(), "Portfolio training returns must vary for every backtest");
    }
}
