#include "quantstats.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/analytics.pb.h>
#include <algorithm>
#include <cmath>

namespace asterion::terminal {
namespace {

class TemporaryExchange
{
public:
    const std::filesystem::path directory =
        std::filesystem::path("/tmp") / ("ast-quantstats-" + unique_process_id());

    TemporaryExchange()
    {
        if (!std::filesystem::create_directory(directory))
            throw Error(ErrorCode::operation_failed, "QuantStats temporary directory unavailable");
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
    }

    ~TemporaryExchange()
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

} // namespace

nlohmann::json
quantstats_report(const nlohmann::json& source, const std::filesystem::path& executable)
{
    if (source.at("kind") != "backtest")
        throw Error(ErrorCode::invalid_request, "QuantStats requires a completed backtest result");
    if (executable.empty() || !std::filesystem::is_regular_file(executable))
        throw Error(
            ErrorCode::unavailable,
            "QuantStats runtime unavailable; prepare or reinstall Terminal");
    analytics::v1::QuantStatsRequest request;
    request.set_task_id(source.at("id").get<std::string>());
    request.set_result_sha256(source.at("task").at("result_digest").get<std::string>());
    request.set_deposit(source.at("experiment").at("paper").at("deposit").get<std::string>());
    for (const auto& row : source.at("result").at("settlements")) {
        auto& value = *request.add_settlements();
        value.set_trading_day(row.at("trading_day").get<std::string>());
        value.set_equity(row.at("equity").get<std::string>());
    }
    TemporaryExchange exchange;
    const auto endpoint = (exchange.directory / "analysis.sock").string();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    const auto remaining = [&] {
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (duration <= std::chrono::milliseconds::zero())
            throw Error(ErrorCode::operation_failed, "QuantStats analysis timed out");
        return duration;
    };
    ChildProcess process(executable, {endpoint});
    // The frozen interpreter binds its endpoint after importing the locked
    // libraries. Wait only for this owned child's startup; never replay requests.
    while (!std::filesystem::exists(endpoint)) {
        if (process.wait(std::min(remaining(), std::chrono::milliseconds(10))))
            throw Error(ErrorCode::operation_failed, "QuantStats analysis process failed");
    }
    auto channel = ipc::Channel::connect(endpoint, remaining());
    channel.send(request.SerializeAsString(), remaining());
    const auto payload = channel.receive(remaining());
    channel.close();
    if (!process.wait(remaining()))
        throw Error(ErrorCode::operation_failed, "QuantStats analysis timed out");
    if (process.exit_code() != 0)
        throw Error(ErrorCode::operation_failed, "QuantStats analysis process failed");
    analytics::v1::QuantStatsReply reply;
    if (payload.size() > 16 * 1024 * 1024 || !reply.ParseFromString(payload))
        throw Error(ErrorCode::operation_failed, "QuantStats returned an invalid report");
    protocol::validate_message(reply);
    if (reply.has_error())
        throw_remote_error(reply.error().code(), reply.error().message());
    const auto& report = reply.report();
    if (!reply.has_report() || report.task_id() != request.task_id()
        || report.result_sha256() != request.result_sha256() || report.periods_per_year() != 252
        || report.library_version() != "0.0.86"
        || report.returns_size() != request.settlements_size())
        throw Error(ErrorCode::operation_failed, "QuantStats returned an invalid report");
    nlohmann::json result = {
        {"task_id", report.task_id()},
        {"result_sha256", report.result_sha256()},
        {"library_version", report.library_version()},
        {"periods_per_year", report.periods_per_year()},
        {"returns", nlohmann::json::array()},
        {"metrics", nlohmann::json::object()}
    };
    for (int index = 0; index < report.returns_size(); ++index) {
        const auto& row = report.returns(index);
        if (row.trading_day() != request.settlements(index).trading_day()
            || !std::isfinite(row.value()))
            throw Error(ErrorCode::operation_failed, "QuantStats returned an invalid report");
        result["returns"].push_back({{"trading_day", row.trading_day()}, {"value", row.value()}});
    }
    const auto metric = [&](const char* name, bool present, double value) {
        if (present && !std::isfinite(value))
            throw Error(ErrorCode::operation_failed, "QuantStats returned an invalid report");
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

} // namespace asterion::terminal
