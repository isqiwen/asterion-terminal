#include "analytics.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/analytics.pb.h>
#include <algorithm>
#include <cmath>
#include <set>
#include <asterion/domain/trading_days.hpp>

namespace asterion::terminal {
namespace {

class TemporaryExchange
{
public:
    const std::filesystem::path directory =
        std::filesystem::path("/tmp") / ("ast-analytics-" + unique_process_id());

    TemporaryExchange()
    {
        if (!std::filesystem::create_directory(directory))
            throw Error(ErrorCode::operation_failed, "Analytics temporary directory unavailable");
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
    }

    ~TemporaryExchange()
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

analytics::proto::BacktestEquity equity_input(const nlohmann::json& source)
{
    if (source.at("kind") != "backtest")
        throw Error(ErrorCode::invalid_request, "Analytics requires a completed backtest result");
    analytics::proto::BacktestEquity request;
    request.set_task_id(source.at("id").get<std::string>());
    request.set_result_sha256(source.at("task").at("result_digest").get<std::string>());
    request.set_deposit(source.at("experiment").at("paper").at("deposit").get<std::string>());
    for (const auto& row : source.at("result").at("settlements")) {
        auto& value = *request.add_settlements();
        value.set_trading_day(row.at("trading_day").get<std::string>());
        value.set_equity(row.at("equity").get<std::string>());
    }
    return request;
}

analytics::proto::AnalyticsReply
analyze(const analytics::proto::AnalyticsRequest& request, const std::filesystem::path& executable)
{
    if (executable.empty() || !std::filesystem::is_regular_file(executable))
        throw Error(
            ErrorCode::unavailable,
            "Analytics runtime unavailable; prepare or reinstall Terminal");
    TemporaryExchange exchange;
    const auto endpoint = (exchange.directory / "analysis.sock").string();
    // macOS's first load of the frozen analytics libraries can exceed 60 seconds.
    // Startup has its own budget; analysis keeps the 30-second IPC deadline.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    const auto remaining = [&] {
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (duration <= std::chrono::milliseconds::zero())
            throw Error(ErrorCode::operation_failed, "Analytics timed out");
        return duration;
    };
    ChildProcess process(executable, {endpoint});
    // The frozen interpreter binds its endpoint after importing the locked
    // libraries. Wait only for this owned child's startup; never replay requests.
    while (!std::filesystem::exists(endpoint)) {
        if (process.wait(std::min(remaining(), std::chrono::milliseconds(10))))
            throw Error(ErrorCode::operation_failed, "Analytics process failed");
    }
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto channel = ipc::Channel::connect(endpoint, remaining());
    channel.send(request.SerializeAsString(), remaining());
    const auto payload = channel.receive(remaining());
    channel.close();
    if (!process.wait(remaining()))
        throw Error(ErrorCode::operation_failed, "Analytics timed out");
    if (process.exit_code() != 0)
        throw Error(ErrorCode::operation_failed, "Analytics process failed");
    analytics::proto::AnalyticsReply reply;
    if (payload.size() > 16 * 1024 * 1024 || !reply.ParseFromString(payload))
        throw Error(ErrorCode::operation_failed, "Analytics returned an invalid report");
    protocol::validate_message(reply);
    if (reply.has_error())
        throw_remote_error(reply.error().code(), reply.error().message());
    return reply;
}

} // namespace

nlohmann::json
quantstats_report(const nlohmann::json& source, const std::filesystem::path& executable)
{
    analytics::proto::AnalyticsRequest message;
    *message.mutable_quantstats() = equity_input(source);
    const auto& request = message.quantstats();
    const auto reply = analyze(message, executable);
    const auto& report = reply.quantstats();
    if (!reply.has_quantstats() || report.task_id() != request.task_id()
        || report.result_sha256() != request.result_sha256() || report.periods_per_year() != 252
        || report.library_version() != "0.0.86" || report.polars_version() != "2.0.0"
        || report.returns_size() != request.settlements_size())
        throw Error(ErrorCode::operation_failed, "Analytics returned an invalid report");
    nlohmann::json result = {
        {"task_id", report.task_id()},
        {"result_sha256", report.result_sha256()},
        {"library_version", report.library_version()},
        {"polars_version", report.polars_version()},
        {"periods_per_year", report.periods_per_year()},
        {"returns", nlohmann::json::array()},
        {"metrics", nlohmann::json::object()}
    };
    for (int index = 0; index < report.returns_size(); ++index) {
        const auto& row = report.returns(index);
        if (row.trading_day() != request.settlements(index).trading_day()
            || !std::isfinite(row.value()))
            throw Error(ErrorCode::operation_failed, "Analytics returned an invalid report");
        result["returns"].push_back({{"trading_day", row.trading_day()}, {"value", row.value()}});
    }
    const auto metric = [&](const char* name, bool present, double value) {
        if (present && !std::isfinite(value))
            throw Error(ErrorCode::operation_failed, "Analytics returned an invalid report");
        result["metrics"][name] = present ? nlohmann::json(value) : nlohmann::json(nullptr);
    };
    metric("sortino", report.has_sortino(), report.sortino());
    metric("omega", report.has_omega(), report.omega());
    metric("tail_ratio", report.has_tail_ratio(), report.tail_ratio());
    metric("ulcer_index", report.has_ulcer_index(), report.ulcer_index());
    metric("value_at_risk", report.has_value_at_risk(), report.value_at_risk());
    metric(
        "conditional_value_at_risk",
        report.has_conditional_value_at_risk(),
        report.conditional_value_at_risk());
    return result;
}

nlohmann::json portfolio_report(
    const std::vector<nlohmann::json>& sources,
    const std::string& training_end,
    const std::filesystem::path& executable)
{
    if (sources.size() < 2 || sources.size() > 3)
        throw Error(
            ErrorCode::invalid_request,
            "Portfolio analysis requires 2 or 3 distinct backtests");
    parse_trading_date(training_end);
    analytics::proto::AnalyticsRequest message;
    auto& request = *message.mutable_portfolio();
    request.set_training_end(training_end);
    std::set<std::string> ids;
    for (const auto& source : sources) {
        auto input = equity_input(source);
        if (!ids.insert(input.task_id()).second)
            throw Error(
                ErrorCode::invalid_request,
                "Portfolio analysis requires 2 or 3 distinct backtests");
        *request.add_backtests() = std::move(input);
    }
    const auto reply = analyze(message, executable);
    const auto& report = reply.portfolio();
    const auto invalid = [] {
        throw Error(ErrorCode::operation_failed, "Analytics returned an invalid report");
    };
    if (!reply.has_portfolio() || report.polars_version() != "2.0.0"
        || report.skfolio_version() != "1.9.0" || report.training_end() != training_end
        || report.weights_size() != request.backtests_size() || report.training_observations() < 20
        || report.validation_size() < 20 || report.training_begin() > report.training_last()
        || report.training_last() > training_end)
        invalid();
    parse_trading_date(report.training_begin());
    parse_trading_date(report.training_last());
    nlohmann::json result = {
        {"polars_version", report.polars_version()},
        {"skfolio_version", report.skfolio_version()},
        {"training_end", training_end},
        {"training_begin", report.training_begin()},
        {"training_last", report.training_last()},
        {"training_observations", report.training_observations()},
        {"weights", nlohmann::json::array()},
        {"validation", nlohmann::json::array()}
    };
    double sum = 0;
    for (int i = 0; i < report.weights_size(); ++i) {
        const auto& weight = report.weights(i);
        const auto& source = request.backtests(i);
        if (weight.task_id() != source.task_id() || weight.result_sha256() != source.result_sha256()
            || weight.source_observations() != static_cast<unsigned>(source.settlements_size())
            || weight.source_observations()
                < report.training_observations() + report.validation_size()
            || !std::isfinite(weight.weight()) || weight.weight() < -1e-8
            || weight.weight() > 1 + 1e-8)
            invalid();
        sum += weight.weight();
        result["weights"].push_back(
            {{"task_id", weight.task_id()},
                {"result_sha256", weight.result_sha256()},
                {"weight", weight.weight()},
                {"source_observations", weight.source_observations()}});
    }
    if (std::abs(sum - 1) > 1e-6)
        invalid();
    auto previous = training_end;
    for (const auto& row : report.validation()) {
        parse_trading_date(row.trading_day());
        if (row.trading_day() <= previous || !std::isfinite(row.optimized_return())
            || !std::isfinite(row.equal_weight_return()))
            invalid();
        previous = row.trading_day();
        result["validation"].push_back(
            {{"trading_day", row.trading_day()},
                {"optimized_return", row.optimized_return()},
                {"equal_weight_return", row.equal_weight_return()}});
    }
    const auto metric = [&](const char* key, double value) {
        if (!std::isfinite(value))
            invalid();
        result[key] = value;
    };
    metric("optimized_total_return", report.optimized_total_return());
    metric("equal_weight_total_return", report.equal_weight_total_return());
    metric("optimized_annual_volatility", report.optimized_annual_volatility());
    metric("equal_weight_annual_volatility", report.equal_weight_annual_volatility());
    return result;
}

} // namespace asterion::terminal
