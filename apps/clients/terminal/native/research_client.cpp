#include <asterion/research.pb.h>
#include "research_client.hpp"
#include <asterion/protocol/resource.hpp>
#include <asterion/protocol/research_project.hpp>
#include <asterion/protocol/research_variable.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/research_logic.hpp>
#include <asterion/protocol/research_batch.hpp>
#include <asterion/protocol/research_assessment.hpp>
#include <asterion/protocol/research_model.hpp>
#include "performance.hpp"
#include "account_reconciliation.hpp"
#include "portfolio_budget.hpp"
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/kernel/process/child.hpp>
#include <optional>
#include <algorithm>
#include <array>
#include <map>

namespace asterion::terminal {

using namespace std::chrono_literals;
namespace wire = task::proto;

namespace {

Json figures(const Performance& value)
{
    const auto optional = [](const std::optional<double>& statistic) {
        return statistic ? Json(*statistic) : Json(nullptr);
    };
    const auto reason = [](PerformanceUnavailable state) {
        return state == PerformanceUnavailable::none
            ? Json(nullptr)
            : Json(std::string(performance_unavailable_name(state)));
    };
    const auto& context = value.context;
    return {
        {"calculation",
            {{"begin_ns",
                 context.begin_ns ? Json(std::to_string(*context.begin_ns)) : Json(nullptr)},
                {"end_ns", context.end_ns ? Json(std::to_string(*context.end_ns)) : Json(nullptr)},
                {"calendar_year_days", 365.2425},
                {"annual_periods",
                    context.annual_periods ? Json(*context.annual_periods) : Json(nullptr)},
                {"risk_free_annual", context.explicitly_zero_risk_free ? Json(0.0) : Json(nullptr)},
                {"frequency", "daily_settlement"},
                {"opening_return_included", context.opening_is_full_period},
                {"return_periods", value.return_periods},
                {"short_calendar_span", value.short_calendar_span},
                {"derived_on_read", true},
                {"unavailable",
                    {{"annual_return", reason(value.annual_return_reason)},
                        {"annual_volatility", reason(value.annual_volatility_reason)},
                        {"sharpe", reason(value.sharpe_reason)},
                        {"calmar", reason(value.calmar_reason)}}}}},
        {"trading_days", value.trading_days},
        {"total_return", value.total_return},
        {"max_drawdown", value.max_drawdown},
        {"winning_days", value.winning_days},
        {"annual_return", optional(value.annual_return)},
        {"annual_volatility", optional(value.annual_volatility)},
        {"sharpe", optional(value.sharpe)},
        {"calmar", optional(value.calmar)}
    };
}

// How a backtest's fixed result performed: over all of its days; for a
// comparison before a holdout, before and from it; for a rolling one, over
// each round's validation days and over all of them together. A span of days
// starts from the equity the day before it ended with. Derived on every read,
// never stored.
Json backtest_performance(
    const backtest::proto::BacktestInput& input,
    const backtest::proto::BacktestResult& result)
{
    std::vector<EquityDay> days;
    for (const auto& day : result.settlements())
        days.push_back(
            {std::chrono::sys_days(parse_trading_date(day.trading_day())),
                Decimal::from_raw(day.equity().units())});
    // Every equity observation in order, and where each day's end.
    std::vector<Decimal> marks;
    std::vector<std::size_t> day_end;
    for (const auto& point : result.equity()) {
        marks.push_back(Decimal::from_raw(point.equity().units()));
        if (point.event() == backtest::proto::DAILY_SETTLEMENT)
            day_end.push_back(marks.size());
    }
    const auto deposit = Decimal::from_raw(input.paper().deposit().units());
    // The figures of days [first, end); none once the account they start from
    // is empty, which has no return to speak of.
    const auto span_of = [&](std::size_t first, std::size_t end) -> Json {
        const auto base = first ? days[first - 1].equity : deposit;
        if (base <= Decimal{})
            return nullptr;
        const auto begin = first ? day_end[first - 1] : 0;
        PerformanceContext context;
        context.begin_ns = first ? result.settlements(static_cast<int>(first - 1)).timestamp_ns()
                                 : result.equity(0).timestamp_ns();
        context.end_ns = result.settlements(static_cast<int>(end - 1)).timestamp_ns();
        context.opening_is_full_period = first > 0;
        const auto& assumptions = input.performance_assumptions();
        if (assumptions.has_annual_periods())
            context.annual_periods = assumptions.annual_periods();
        context.explicitly_zero_risk_free =
            assumptions.risk_free() == backtest::proto::PerformanceAssumptions::EXPLICIT_ZERO;
        return figures(performance(
            base,
            std::span(days).subspan(first, end - first),
            std::span(marks).subspan(begin, day_end[end - 1] - begin),
            context));
    };
    auto value = span_of(0, days.size());
    std::vector<SettlementDay> settlements;
    for (const auto& day : result.settlements())
        settlements.push_back(
            {std::chrono::sys_days(parse_trading_date(day.trading_day())),
                Decimal::from_raw(day.equity().units()),
                Decimal::from_raw(day.fees().units())});
    value["by_month"] = Json::array();
    for (const auto& month : monthly_contributions(deposit, settlements))
        value["by_month"].push_back(
            {{"first_day", format_trading_date(std::chrono::year_month_day{month.first_day})},
                {"last_day", format_trading_date(std::chrono::year_month_day{month.last_day})},
                {"trading_days", month.trading_days},
                {"opening_equity", month.opening_equity.str()},
                {"closing_equity", month.closing_equity.str()},
                {"gross", month.gross.str()},
                {"fees", month.fees.str()},
                {"net", month.net.str()},
                {"return_contribution", month.return_contribution}});
    value["development"] = nullptr;
    value["holdout"] = nullptr;
    value["out_of_sample"] = nullptr;
    value["folds"] = Json::array();
    if (!input.holdout_day().empty()) {
        const auto development = static_cast<std::size_t>(
            std::ranges::count_if(result.settlements(), [&](const auto& day) {
                return day.trading_day() < input.holdout_day();
            }));
        value["development"] = span_of(0, development);
        value["holdout"] = span_of(development, days.size());
    }
    if (input.has_walk_forward()) {
        const auto rounds = protocol::backtest_folds(input);
        value["out_of_sample"] = span_of(rounds.front().first, days.size());
        for (const auto& round : rounds)
            value["folds"].push_back(span_of(round.first, round.end));
    }
    return value;
}

// What a backtest's closed positions say, from its fixed fills. Derived on
// every read like the figures above.
Json backtest_trades(
    const backtest::proto::BacktestInput& input,
    const backtest::proto::BacktestResult& result)
{
    const auto& contracts = input.paper().contracts();
    std::vector<Decimal> multipliers;
    for (const auto& contract : contracts)
        multipliers.push_back(
            Decimal::from_raw(contract.dataset().contract().multiplier().units()));
    // A fill says which order it filled; the order says which way.
    std::map<std::string, bool> buys;
    for (const auto& order : result.account().orders())
        buys[order.id()] = order.side() == protocol::proto::BUY;
    std::vector<TradedFill> fills;
    for (const auto& fill : result.account().fills()) {
        const auto contract = std::ranges::find_if(contracts, [&](const auto& item) {
            return item.dataset().contract().venue() == fill.venue()
                && item.dataset().contract().symbol() == fill.symbol();
        });
        fills.push_back(
            {static_cast<std::size_t>(contract - contracts.begin()),
                buys.at(fill.order_id()),
                Decimal::from_raw(fill.quantity().units()),
                Decimal::from_raw(fill.price().units()),
                Decimal::from_raw(fill.fee().units())});
    }
    const auto value = trades(fills, multipliers);
    const auto optional = [](const std::optional<double>& statistic) {
        return statistic ? Json(*statistic) : Json(nullptr);
    };
    const auto holding =
        [&](std::size_t contract, std::size_t first_fill, const std::string& last) -> Json {
        if (result.fill_days().empty())
            return nullptr;
        const auto& begin = result.fill_days(static_cast<int>(first_fill)).trading_day();
        const auto& days = contracts[static_cast<int>(contract)].dataset().days();
        const auto find_day = [&](const auto& day) {
            return std::lower_bound(
                days.begin(),
                days.end(),
                day,
                [](const auto& item, const auto& label) {
                    return item.trading_day() < label;
                });
        };
        const auto first = find_day(begin), end = find_day(last);
        if (first == days.end() || end == days.end() || first->trading_day() != begin
            || end->trading_day() != last || end < first)
            throw std::invalid_argument("invalid backtest fill-day evidence");
        const auto calendar =
            (std::chrono::sys_days(parse_trading_date(last))
                - std::chrono::sys_days(parse_trading_date(begin)))
                .count();
        const auto settlements = end - first;
        return {
            {"opened_day", begin},
            {"ended_day", last},
            {"calendar_days", calendar},
            {"settlement_days", settlements}
        };
    };
    Json closed = Json::array(), open = Json::array();
    std::vector<double> calendar_spans, settlement_spans;
    Decimal gross, fees, net;
    for (const auto& trade : value.closed) {
        const auto& contract = contracts[static_cast<int>(trade.contract)].dataset().contract();
        closed.push_back(
            {{"venue", contract.venue()},
                {"symbol", contract.symbol()},
                {"gross", trade.gross.str()},
                {"fees", trade.fees.str()},
                {"net", trade.net.str()}});
        auto period = result.fill_days().empty()
            ? Json(nullptr)
            : holding(
                  trade.contract,
                  trade.first_fill,
                  result.fill_days(static_cast<int>(trade.last_fill)).trading_day());
        if (!period.is_null()) {
            calendar_spans.push_back(period.at("calendar_days").get<double>());
            settlement_spans.push_back(period.at("settlement_days").get<double>());
        }
        closed.back()["holding"] = std::move(period);
        gross = gross + trade.gross;
        fees = fees + trade.fees;
        net = net + trade.net;
    }
    for (const auto& trade : value.open) {
        const auto& contract = contracts[static_cast<int>(trade.contract)].dataset().contract();
        open.push_back(
            {{"venue", contract.venue()},
                {"symbol", contract.symbol()},
                {"direction", trade.quantity > Decimal{} ? "long" : "short"},
                {"quantity",
                    (trade.quantity > Decimal{} ? trade.quantity : Decimal{} - trade.quantity)
                        .str()},
                {"fees", trade.fees.str()},
                {"as_of_day", result.settlements(result.settlements_size() - 1).trading_day()},
                {"holding",
                    holding(
                        trade.contract,
                        trade.first_fill,
                        result.settlements(result.settlements_size() - 1).trading_day())}});
    }
    const auto spans = [](const std::vector<double>& values) -> Json {
        if (values.empty())
            return {{"minimum", nullptr}, {"mean", nullptr}, {"maximum", nullptr}};
        double total = 0;
        for (auto value : values)
            total += value;
        return {
            {"minimum", *std::ranges::min_element(values)},
            {"mean", total / values.size()},
            {"maximum", *std::ranges::max_element(values)}
        };
    };
    Json by_contract = Json::array();
    for (const auto& contribution : value.by_contract) {
        const auto& contract =
            contracts[static_cast<int>(contribution.contract)].dataset().contract();
        by_contract.push_back(
            {{"venue", contract.venue()},
                {"symbol", contract.symbol()},
                {"count", contribution.count},
                {"gross", contribution.gross.str()},
                {"fees", contribution.fees.str()},
                {"net", contribution.net.str()}});
    }
    return {
        {"count", value.closed.size()},
        {"by_contract", std::move(by_contract)},
        {"holding_summary",
            {{"known", calendar_spans.size()},
                {"unknown", value.closed.size() - calendar_spans.size()},
                {"calendar_days", spans(calendar_spans)},
                {"settlement_days", spans(settlement_spans)}}},
        {"open", std::move(open)},
        {"closed", std::move(closed)},
        {"gross", gross.str()},
        {"fees", fees.str()},
        {"net", net.str()},
        {"open_fees", value.open_fees.str()},
        {"winning", value.winning},
        {"losing", value.losing},
        {"average_win", optional(value.average_win)},
        {"average_loss", optional(value.average_loss)},
        {"payoff", optional(value.payoff)}
    };
}

Json backtest_portfolio(
    const backtest::proto::BacktestInput& input,
    const backtest::proto::BacktestResult& result)
{
    const auto exposure = portfolio_exposure(input, result);
    Json rows = Json::array();
    for (const auto& row : exposure.contracts) {
        const auto share = exposure.gross_notional == Decimal{}
            ? Json(nullptr)
            : Json(
                  static_cast<double>(
                      divide(row.gross_notional, exposure.gross_notional, Rounding::half_up).raw())
                  / 100000000.0);
        rows.push_back(
            {{"venue", row.venue},
                {"symbol", row.symbol},
                {"mark", row.mark.str()},
                {"multiplier", row.multiplier.str()},
                {"quantity_increment", row.quantity_increment.str()},
                {"net_quantity", row.net_quantity.str()},
                {"gross_quantity", row.gross_quantity.str()},
                {"net_notional", row.net_notional.str()},
                {"gross_notional", row.gross_notional.str()},
                {"gross_notional_share", share},
                {"margin", row.margin.str()},
                {"cost_source", row.cost_source},
                {"cost_effective_from", row.cost_effective_from},
                {"margin_per_lot", row.costs.margin_per_lot.str()},
                {"margin_rate", row.costs.margin_rate.str()},
                {"quantities_on_grid", row.quantities_on_grid}});
    }
    Json points = Json::array();
    const auto optional = [](const std::optional<Decimal>& value) {
        return value ? Json(value->str()) : Json(nullptr);
    };
    for (const auto& point : settlement_exposure(input, result))
        points.push_back(
            {{"day", point.day},
                {"net_notional", point.net_notional.str()},
                {"gross_notional", optional(point.gross_notional)},
                {"model_margin", optional(point.margin)},
                {"balance", point.balance.str()},
                {"equity", point.equity.str()},
                {"available", optional(point.available)},
                {"frozen", optional(point.frozen)}});
    return {
        {"settlement_path",
            {{"scope", "recorded_settlement_points"}, {"points", std::move(points)}}},
        {"scope", "final_saved_account"},
        {"contracts", std::move(rows)},
        {"net_notional", exposure.net_notional.str()},
        {"gross_notional", exposure.gross_notional.str()},
        {"gross_quantity", exposure.gross_quantity.str()},
        {"model_margin", exposure.margin.str()},
        {"recorded_margin", exposure.recorded_margin.str()},
        {"margin_difference", (exposure.recorded_margin - exposure.margin).str()},
        {"balance", exposure.balance.str()},
        {"equity", exposure.equity.str()},
        {"available", exposure.available.str()},
        {"frozen", exposure.frozen.str()},
        {"risk",
            {{"max_order_quantity", exposure.max_order_quantity.str()},
                {"max_gross_quantity", exposure.max_gross_quantity.str()},
                {"max_working_orders", exposure.max_working_orders},
                {"final_lot_headroom",
                    (exposure.max_gross_quantity - exposure.gross_quantity).str()}}}
    };
}

Json backtest_reconciliation(
    const backtest::proto::BacktestInput& input,
    const backtest::proto::BacktestResult& result)
{
    const auto audit = reconcile_backtest(input, result);
    const auto checks = [](const std::vector<AccountCheck>& items) {
        Json rows = Json::array();
        for (const auto& item : items)
            rows.push_back(
                {{"metric", item.metric},
                    {"expected", item.expected.str()},
                    {"recorded", item.recorded.str()},
                    {"difference", (item.recorded - item.expected).str()},
                    {"matches", item.recorded == item.expected}});
        return rows;
    };
    Json value{
        {"state",
            audit.state == AccountReconciliation::State::available ? "available"
                : audit.state == AccountReconciliation::State::missing_fill_days
                ? "missing_fill_days"
                : "unsupported_input"},
        {"days", Json::array()},
        {"final_checks", checks(audit.final_checks)}
    };
    if (audit.state != AccountReconciliation::State::available)
        return value;
    value["model"] = {
        {"venue", audit.venue},
        {"symbol", audit.symbol},
        {"deposit", audit.deposit.str()},
        {"multiplier", audit.multiplier.str()},
        {"cost_source", audit.cost_source},
        {"cost_effective_from", audit.cost_effective_from},
        {"margin_per_lot", audit.costs.margin_per_lot.str()},
        {"margin_rate", audit.costs.margin_rate.str()},
        {"signed_notional", audit.signed_notional.str()},
        {"gross_notional", audit.gross_notional.str()}
    };
    for (const auto& day : audit.days) {
        Json fills = Json::array();
        for (const auto& fill : day.fills)
            fills.push_back(
                {{"id", fill.id},
                    {"order_id", fill.order_id},
                    {"side", fill.buy ? "buy" : "sell"},
                    {"quantity", fill.quantity.str()},
                    {"price", fill.price.str()},
                    {"fee", fill.fee.str()}});
        value["days"].push_back(
            {{"day", day.day},
                {"opening_quantity", day.opening_quantity.str()},
                {"quantity_change", day.quantity_change.str()},
                {"closing_quantity", day.closing_quantity.str()},
                {"settlement", day.settlement.str()},
                {"opening_balance", day.opening_balance.str()},
                {"gross", day.gross.str()},
                {"fees", day.fees.str()},
                {"expected_balance", day.expected_balance.str()},
                {"recorded_balance", day.recorded_balance.str()},
                {"fills", std::move(fills)},
                {"checks", checks(day.checks)}});
    }
    return value;
}

wire::TaskResponse
decode_task_response(const research::proto::ResearchRequest& request, const std::string& raw)
{
    wire::TaskResponse response;
    if (!response.ParseFromString(raw))
        throw Error(ErrorCode::unavailable, "invalid task response");
    protocol::validate_message(response);
    if (response.version() != 1 || response.service_id() != request.service_id()
        || response.correlation_id() != request.correlation_id())
        throw Error(ErrorCode::unavailable, "task response identity mismatch");
    if (response.has_error())
        throw_remote_error(response.error().code(), response.error().message());
    if (request.has_assessment()          ? !response.has_assessment()
            : request.has_batch()         ? !response.has_batch()
            : request.has_logic()         ? !response.has_logic()
            : request.has_model()         ? !response.has_model()
            : request.has_strategy()      ? !response.has_strategy()
            : request.has_variable()      ? !response.has_variable()
            : request.has_project()       ? !response.has_project()
            : request.has_project_tasks() ? !response.has_project_tasks()
            : request.has_resources()     ? !response.has_resources()
            : request.has_retirement()    ? !response.has_retirement()
            : request.has_history_usage() ? !response.has_history_usage()
            : request.has_list()          ? !response.has_tasks()
            : request.has_result()
            ? (!response.has_backtest() && !response.has_factor() && !response.has_minutes()
                  && !response.has_table_update() && !response.has_account_review()
                  && !response.has_statistics() && !response.has_sample()
                  && !response.has_logic_trial() && !response.has_model_training()
                  && !response.has_model_time() && !response.has_evaluation())
            : !response.has_task())
        throw Error(ErrorCode::unavailable, "unexpected task response");
    return response;
}

Json decode_task_page(const research::proto::ResearchRequest& request, const std::string& raw)
{
    const auto response = decode_task_response(request, raw);
    return protocol::decode_task_page(response.tasks(), request.list().before_sequence());
}

} // namespace

struct ResearchClient::Impl : std::enable_shared_from_this<Impl>
{
    ServiceIo& io;
    const ServiceEndpoint endpoint;
    const std::string connection_id = unique_process_id();
    // Long reads cannot occupy the control or list-observation slots.
    enum class Lane
    {
        read,
        control,
        observation
    };
    std::array<std::unique_ptr<ipc::RpcClient>, 3> transports;
    std::stop_source lifetime;
    bool refreshing = false;
    std::shared_ptr<const Json> page = std::make_shared<const Json>(Json{
        {"capacity", nullptr},
        {"before_sequence", 0},
        {"next_before_sequence", 0},
        {"failed_count", 0},
        {"interrupted_count", 0},
        {"tasks", Json::array()}
    });
    bool online = false;
    std::string error;

    Read view() const
    {
        return {
            page,
            Json{
                {"connection_id", connection_id},
                {"node_id", endpoint.node_id},
                {"owner_node_id", endpoint.owner_node_id},
                {"service", endpoint.session},
                {"port", endpoint.port},
                {"host", endpoint.endpoint.empty() ? endpoint.host : "localhost"},
                {"remote", endpoint.endpoint.empty()},
                {"online", online},
                {"error", error}
            }
        };
    }

    Impl(ServiceIo& owner, ServiceEndpoint value)
        : io(owner),
          endpoint(std::move(value))
    {
        validate_id(endpoint.session);
        // Prepare TLS files on the creating management operation before handoff to I/O.
        constexpr std::array<std::size_t, 3> slots{8, 4, 1};
        for (std::size_t i = 0; i < transports.size(); ++i) {
            auto budget = io.payload_budget(
                i == static_cast<std::size_t>(Lane::read) ? ServiceIo::PayloadLane::data
                                                          : ServiceIo::PayloadLane::control);
            transports[i] = endpoint.endpoint.empty() ? std::make_unique<ipc::RpcClient>(
                                                            io.reactor(),
                                                            endpoint.host,
                                                            endpoint.port,
                                                            endpoint.tls,
                                                            slots[i],
                                                            budget)
                                                      : std::make_unique<ipc::RpcClient>(
                                                            io.reactor(),
                                                            endpoint.endpoint,
                                                            slots[i],
                                                            budget);
        }
    }

    bool stopped(std::stop_token stop) const
    {
        return stop.stop_requested() || lifetime.stop_requested();
    }

    PolledTask<Payload> exchange(
        std::string request,
        std::chrono::milliseconds timeout,
        Lane lane,
        std::stop_token stop,
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max(),
        std::chrono::milliseconds send_timeout = 5s)
    {
        if (stopped(stop))
            throw Error(ErrorCode::cancelled, "task connection closed");
        auto& transport = transports[static_cast<std::size_t>(lane)];
        auto reply = transport->request(std::move(request), timeout, deadline, send_timeout);
        while (reply.wait_for(0ms) != std::future_status::ready) {
            if (stopped(stop))
                transport.reset();
            else
                transport->poll();
            co_await std::suspend_always{};
        }
        co_return reply.get();
    }

    void identify(research::proto::ResearchRequest& request) const
    {
        request.set_version(1);
        request.set_service_id(endpoint.session);
        request.set_correlation_id(next_correlation_id());
    }

    PolledTask<Payload> request_bytes(
        research::proto::ResearchRequest& request,
        std::stop_token stop,
        std::optional<std::chrono::steady_clock::time_point> deadline = {})
    {
        identify(request);
        std::chrono::milliseconds timeout = request.has_result() || request.has_strategy()
                || (request.has_model() && request.model().has_mutation()
                    && (request.model().mutation().has_fix()
                        || request.model().mutation().has_time_validation()))
                || (request.has_batch() && request.batch().has_mutation()
                    && request.batch().mutation().has_save())
                || (request.has_assessment()
                    && (request.assessment().has_preview() || request.assessment().has_mutation()))
            ? protocol::task_verification_timeout + 5s
            : request.has_project() || request.has_project_tasks() || request.has_variable()
                || request.has_logic() || request.has_batch() || request.has_model()
                || request.has_assessment()
            ? 10s
            : request.has_resources()  ? 30s
            : request.has_retirement() ? 15s
            : request.has_get()        ? protocol::task_input_timeout + 5s
            : request.has_history_usage() || request.has_submit() ? 60s
                                                                  : 5s;
        if (deadline) {
            timeout = std::min(
                timeout,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    *deadline - std::chrono::steady_clock::now()));
            if (timeout <= 0ms)
                throw Error(ErrorCode::unavailable, "task reference inspection timed out");
        }
        const auto lane = request.has_result() || request.has_get() || request.has_metadata_get()
                || request.has_history_usage() || request.has_retirement()
                || request.has_resources() || request.has_project() || request.has_project_tasks()
                || request.has_strategy() || request.has_variable() || request.has_logic()
                || request.has_model() || request.has_assessment()
                || (request.has_batch()
                    && (!request.batch().has_mutation() || request.batch().mutation().has_save()))
            ? Lane::read
            : Lane::control;
        const auto send_timeout = request.has_submit() || request.has_history_usage() ? 30s : 5s;
        auto bytes = request.has_submit() ? co_await io.read<std::string>([&] {
            return request.SerializeAsString();
        })
                                          : request.SerializeAsString();
        co_return co_await exchange(
            std::move(bytes),
            timeout,
            lane,
            stop,
            deadline.value_or(std::chrono::steady_clock::time_point::max()),
            send_timeout);
    }

    PolledTask<Json> read(
        research::proto::ResearchRequest request,
        std::stop_token stop,
        std::optional<std::chrono::steady_clock::time_point> deadline = {})
    {
        auto raw = co_await request_bytes(request, stop, deadline);
        co_return co_await io.read<Json>([request = std::move(request),
                                             raw = std::move(raw),
                                             node = endpoint.owner_node_id] {
            const auto response = decode_task_response(request, *raw);
            if (request.has_assessment()) {
                if (response.has_result_task())
                    throw Error(ErrorCode::unavailable, "invalid batch assessment response");
                resource::proto::Owner owner;
                owner.set_node_id(node);
                owner.set_service_id(request.service_id());
                owner.set_domain("research");
                return protocol::decode_batch_assessment_reply(
                    response.assessment(),
                    request.assessment(),
                    owner);
            }
            if (request.has_batch()) {
                if (response.has_result_task())
                    throw Error(ErrorCode::unavailable, "invalid research batch response");
                resource::proto::Owner owner;
                owner.set_node_id(node);
                owner.set_service_id(request.service_id());
                owner.set_domain("research");
                return protocol::decode_backtest_batch_reply(
                    response.batch(),
                    request.batch(),
                    owner);
            }
            if (request.has_model()) {
                if (response.has_result_task())
                    throw Error(ErrorCode::unavailable, "invalid model metadata response");
                resource::proto::Owner owner;
                owner.set_node_id(node);
                owner.set_service_id(request.service_id());
                owner.set_domain("research");
                return protocol::decode_model_reply(response.model(), request.model(), owner);
            }
            if (request.has_logic()) {
                if (response.has_result_task())
                    throw Error(ErrorCode::unavailable, "invalid research logic response");
                resource::proto::Owner owner;
                owner.set_node_id(node);
                owner.set_service_id(request.service_id());
                owner.set_domain("research");
                return protocol::decode_logic_reply(response.logic(), request.logic(), owner);
            }
            if (request.has_strategy()) {
                if (!response.has_strategy() || response.has_result_task())
                    throw Error(ErrorCode::unavailable, "invalid research strategy response");
                resource::proto::Owner owner;
                owner.set_node_id(node);
                owner.set_service_id(request.service_id());
                owner.set_domain("research");
                return protocol::decode_strategy_reply(
                    response.strategy(),
                    request.strategy(),
                    owner);
            }
            if (request.has_variable()) {
                if (response.has_result_task())
                    throw Error(ErrorCode::unavailable, "invalid research variable response");
                resource::proto::Owner owner;
                owner.set_node_id(node);
                owner.set_service_id(request.service_id());
                owner.set_domain("research");
                return protocol::decode_variable_reply(
                    response.variable(),
                    request.variable(),
                    owner);
            }
            if (request.has_project()) {
                if (response.has_result_task())
                    throw Error(ErrorCode::unavailable, "invalid research project response");
                resource::proto::Owner owner;
                owner.set_node_id(node);
                owner.set_service_id(request.service_id());
                owner.set_domain("research");
                return protocol::decode_project_reply(response.project(), request.project(), owner);
            }
            if (request.has_project_tasks()) {
                const auto& page = response.project_tasks();
                const auto& query = request.project_tasks();
                if (response.has_result_task()
                    || page.tasks_size() > static_cast<int>(query.limit())
                    || (page.next_before_sequence()
                        && (page.tasks().empty()
                            || page.next_before_sequence() != page.tasks(0).submission_sequence())))
                    throw Error(ErrorCode::unavailable, "invalid research project response");
                Json rows = Json::array();
                unsigned previous = 0;
                for (const auto& task : page.tasks()) {
                    if (task.submission_sequence() <= previous
                        || (query.before_sequence()
                            && task.submission_sequence() >= query.before_sequence())
                        || (task.kind() != wire::FACTOR && task.kind() != wire::SAMPLE
                            && task.kind() != wire::STATISTICS && task.kind() != wire::BACKTEST)
                        || !task.has_research_plan()
                        || task.research_plan().owner().domain() != "research"
                        || task.research_plan().project_id() != query.project_id()
                        || task.research_plan().owner().node_id() != node
                        || task.research_plan().owner().service_id() != request.service_id())
                        throw Error(ErrorCode::unavailable, "invalid research project response");
                    previous = task.submission_sequence();
                    rows.push_back(protocol::decode_task(task));
                }
                return Json{
                    {"tasks", std::move(rows)},
                    {"next_before_sequence", page.next_before_sequence()}
                };
            }
            if (request.has_resources()) {
                resource::proto::Owner owner;
                owner.set_node_id(node);
                owner.set_service_id(request.service_id());
                owner.set_domain("research");
                return protocol::decode_resource_catalogue(
                    response.resources(),
                    request.resources(),
                    owner);
            }
            if (request.has_retirement()) {
                return protocol::decode_task_retirement(
                    response.retirement(),
                    request.retirement().id(),
                    request.service_id(),
                    "research");
            }
            if (request.has_metadata_get())
                return protocol::decode_task(response.task());
            if (request.has_result()) {
                if (response.has_logic_trial()
                    && (response.result_task().logic_trial().owner().node_id() != node
                        || response.result_task().logic_trial().owner().service_id()
                            != request.service_id()))
                    throw Error(ErrorCode::conflict, "invalid research logic owner");
                if (response.has_sample()
                    && (response.sample().owner().node_id() != node
                        || response.sample().owner().service_id() != request.service_id()))
                    throw Error(ErrorCode::conflict, "invalid research sample owner");
                if (response.has_evaluation()
                    && (response.result_task().evaluation().request().source().owner().node_id()
                            != node
                        || response.result_task()
                                .evaluation()
                                .request()
                                .source()
                                .owner()
                                .service_id()
                            != request.service_id()))
                    throw Error(
                        ErrorCode::conflict,
                        "evaluation source must belong to this Research service");
                if (response.has_statistics()
                    && (response.statistics().owner().node_id() != node
                        || response.statistics().owner().service_id() != request.service_id()))
                    throw Error(
                        ErrorCode::conflict,
                        "statistics source must belong to this Research service");
                if (response.has_account_review()
                    && (response.account_review().owner().node_id() != node
                        || response.account_review().owner().service_id() != request.service_id()))
                    throw Error(
                        ErrorCode::conflict,
                        "account review result belongs to another Research owner");
                auto evidence = protocol::decode_task_result(response, request.result().id());
                if (response.has_backtest()) {
                    evidence["portfolio"] =
                        backtest_portfolio(response.result_task().input(), response.backtest());
                    evidence["performance"] =
                        backtest_performance(response.result_task().input(), response.backtest());
                    evidence["trades"] =
                        backtest_trades(response.result_task().input(), response.backtest());
                    evidence["reconciliation"] = backtest_reconciliation(
                        response.result_task().input(),
                        response.backtest());
                }
                return evidence;
            }
            if (response.history_usage().dataset_id() != request.history_usage().id())
                throw Error(ErrorCode::unavailable, "invalid historical usage response");
            return protocol::decode_history_usage(response.history_usage());
        });
    }

    std::future<void> mutate(research::proto::ResearchRequest request)
    {
        return io.submit<void>(
            [state = shared_from_this(), request = std::move(request)](
                std::stop_token stop) mutable -> PolledTask<void> {
                const auto raw = co_await state->request_bytes(request, stop);
                (void)decode_task_response(request, *raw);
                co_await state->refresh(
                    stop,
                    request.has_submit() ? std::optional<unsigned>{0} : std::nullopt);
            });
    }

    PolledTask<void> refresh(std::stop_token stop, std::optional<unsigned> requested_page = {})
    {
        co_await PollUntil{[&] {
            return !refreshing || stopped(stop);
        }};
        if (stopped(stop))
            throw Error(ErrorCode::cancelled, "task connection closed");
        refreshing = true;

        struct Release
        {
            bool& refreshing;

            ~Release()
            {
                refreshing = false;
            }
        } release{refreshing};

        research::proto::ResearchRequest request;
        request.mutable_list()->set_limit(200);
        const auto before = requested_page.value_or(page->at("before_sequence").get<unsigned>());
        request.mutable_list()->set_before_sequence(before);
        identify(request);
        auto raw = co_await exchange(request.SerializeAsString(), 5s, Lane::observation, stop);
        auto prepared = co_await io.read<std::shared_ptr<const Json>>(
            [request = std::move(request), raw = std::move(raw), previous = page] {
                auto next = decode_task_page(request, *raw);
                return next == *previous ? previous : std::make_shared<const Json>(std::move(next));
            },
            ServiceIo::ReadLane::response);
        if (stopped(stop))
            throw Error(ErrorCode::cancelled, "task connection closed");
        page = std::move(prepared);
        online = true;
        error.clear();
    }

    PolledTask<void> initialize(std::stop_token stop)
    {
        try {
            co_await refresh(stop);
        } catch (const Error& e) {
            error = e.what();
        }
    }

    PolledTask<void> monitor(std::stop_token stop)
    {
        while (!stopped(stop)) {
            const auto next = std::chrono::steady_clock::now() + 1s;
            co_await PollUntil{[&] {
                return stopped(stop) || std::chrono::steady_clock::now() >= next;
            }};
            if (stopped(stop))
                break;
            try {
                co_await refresh(stop);
            } catch (const std::exception& e) {
                online = false;
                error = e.what();
            }
        }
        for (auto& transport : transports)
            transport.reset();
    }
};

ResearchClient::ResearchClient(ServiceIo& io, ServiceEndpoint endpoint)
    : impl_(std::make_shared<Impl>(io, std::move(endpoint)))
{}

std::future<std::shared_ptr<ResearchClient>>
ResearchClient::open(ServiceIo& io, ServiceEndpoint endpoint)
{
    return io.submit<std::shared_ptr<ResearchClient>>(
        [&io, endpoint = std::move(endpoint)](
            std::stop_token stop) mutable -> PolledTask<std::shared_ptr<ResearchClient>> {
            auto client = co_await io.admin<std::shared_ptr<ResearchClient>>([&] {
                return std::shared_ptr<ResearchClient>(new ResearchClient(io, std::move(endpoint)));
            });
            co_await client->impl_->initialize(stop);
            (void)client->impl_->io.submit<void>(
                [state = client->impl_](std::stop_token stop) {
                    return state->monitor(stop);
                },
                ServiceIo::Lane::observation);
            co_return client;
        });
}

ResearchClient::~ResearchClient()
{
    impl_->lifetime.request_stop();
}

Json ResearchClient::Read::render() const
{
    auto result = connection;
    result.update(*page);
    return result;
}

ResearchClient::Read ResearchClient::owner_view() const
{
    return impl_->view();
}

std::future<Json> ResearchClient::status() const
{
    return impl_->io.submit<Json>([state = impl_](std::stop_token) -> PolledTask<Json> {
        auto view = state->view();
        co_return co_await state->io.read<Json>(
            [view = std::move(view)] {
                return view.render();
            },
            ServiceIo::ReadLane::response);
    });
}

std::future<void> ResearchClient::page(unsigned before_sequence)
{
    return impl_->io.submit<void>([state = impl_, before_sequence](std::stop_token stop) {
        return state->refresh(stop, before_sequence);
    });
}

std::future<void> ResearchClient::submit(
    const std::string& id,
    const asterion::backtest::proto::BacktestRequest& input,
    const data::proto::DataOwner& owner,
    const research::proto::StrategyUse& use,
    const research::proto::ProjectBinding& plan)
{
    research::proto::ResearchRequest request;
    request.mutable_submit()->set_id(id);
    *request.mutable_submit()->mutable_backtest() = input;
    *request.mutable_submit()->mutable_data_owner() = owner;
    if (!use.package().id().empty())
        *request.mutable_submit()->mutable_strategy_use() = use;
    if (!plan.plan_id().empty())
        *request.mutable_submit()->mutable_research_plan() = plan;
    return impl_->mutate(std::move(request));
}

std::future<void> ResearchClient::submit(
    const std::string& id,
    const asterion::factor::proto::FactorRequest& input,
    const data::proto::DataOwner& owner,
    const wire::TaskDerivation& source,
    const research::proto::ProjectBinding& plan)
{
    research::proto::ResearchRequest request;
    request.mutable_submit()->set_id(id);
    *request.mutable_submit()->mutable_factor_request() = input;
    *request.mutable_submit()->mutable_data_owner() = owner;
    if (!plan.plan_id().empty())
        *request.mutable_submit()->mutable_research_plan() = plan;
    if (!source.source_id().empty())
        *request.mutable_submit()->mutable_derivation() = source;
    return impl_->mutate(std::move(request));
}

std::future<void> ResearchClient::submit(
    const std::string& id,
    const research::proto::SampleRequest& input,
    const data::proto::DataOwner& source,
    const research::proto::ProjectBinding& plan)
{
    research::proto::ResearchRequest request;
    request.mutable_submit()->set_id(id);
    *request.mutable_submit()->mutable_sample() = input;
    if (!plan.plan_id().empty())
        *request.mutable_submit()->mutable_research_plan() = plan;
    *request.mutable_submit()->mutable_data_owner() = source;
    return impl_->mutate(std::move(request));
}

std::future<void>
ResearchClient::submit(const std::string& id, const research::proto::EvaluationRequest& input)
{
    research::proto::ResearchRequest request;
    request.mutable_submit()->set_id(id);
    *request.mutable_submit()->mutable_evaluation() = input;
    return impl_->mutate(std::move(request));
}

std::future<void> ResearchClient::submit(
    const std::string& id,
    const research::proto::StatisticsRequest& input,
    const research::proto::ProjectBinding& plan)
{
    research::proto::ResearchRequest request;
    request.mutable_submit()->set_id(id);
    *request.mutable_submit()->mutable_statistics() = input;
    if (!plan.plan_id().empty())
        *request.mutable_submit()->mutable_research_plan() = plan;
    return impl_->mutate(std::move(request));
}

std::future<void>
ResearchClient::submit(const std::string& id, const research::proto::AccountReviewRequest& input)
{
    research::proto::ResearchRequest request;
    request.mutable_submit()->set_id(id);
    *request.mutable_submit()->mutable_account_review() = input;
    return impl_->mutate(std::move(request));
}

std::future<nlohmann::json> ResearchClient::metadata(const std::string& id)
{
    research::proto::ResearchRequest request;
    request.mutable_metadata_get()->set_id(id);
    return impl_->io.submit<Json>(
        [state = impl_, request = std::move(request)](
            std::stop_token stop) mutable -> PolledTask<Json> {
            co_return co_await state->read(std::move(request), stop);
        });
}

std::future<void> ResearchClient::edit_metadata(const wire::TaskMetadataEdit& edit)
{
    research::proto::ResearchRequest request;
    *request.mutable_metadata_edit() = edit;
    return impl_->mutate(std::move(request));
}

std::future<void> ResearchClient::reuse_backtest(
    const std::string& id,
    const backtest::proto::BacktestReuse& reuse,
    const data::proto::DataOwner& owner,
    const research::proto::StrategyUse& use)
{
    research::proto::ResearchRequest request;
    request.mutable_submit()->set_id(id);
    *request.mutable_submit()->mutable_backtest_reuse() = reuse;
    *request.mutable_submit()->mutable_data_owner() = owner;
    if (use.has_package() || use.has_delivery()) {
        protocol::validate_strategy_use(use);
        *request.mutable_submit()->mutable_strategy_use() = use;
    }
    return impl_->mutate(std::move(request));
}

ServiceEndpoint ResearchClient::endpoint() const
{
    return impl_->endpoint;
}

std::future<Json> ResearchClient::retirement(const std::string& id, bool execute)
{
    research::proto::ResearchRequest request;
    request.mutable_retirement()->set_id(id);
    request.mutable_retirement()->set_execute(execute);
    return impl_->io.submit<Json>(
        [state = impl_, request = std::move(request)](
            std::stop_token stop) mutable -> PolledTask<Json> {
            auto result = co_await state->read(request, stop);
            if (result.at("node") != state->endpoint.owner_node_id)
                throw Error(ErrorCode::unavailable, "Research retirement owner identity mismatch");
            if (request.retirement().execute())
                co_await state->refresh(stop);
            co_return result;
        });
}

std::future<Json> ResearchClient::history_usage(const std::string& id)
{
    research::proto::ResearchRequest request;
    request.mutable_history_usage()->set_id(id);
    return impl_->io.submit<Json>(
        [state = impl_, request = std::move(request)](std::stop_token stop) {
            return state->read(request, stop);
        });
}

std::future<Json> ResearchClient::inspect_history_usage(
    ServiceIo& io,
    const ServiceEndpoint& endpoint,
    const std::string& id,
    std::chrono::steady_clock::time_point deadline)
{
    if (endpoint.session.empty()
        || (endpoint.endpoint.empty() && (endpoint.host.empty() || !endpoint.port)))
        throw std::invalid_argument("invalid historical service address");
    research::proto::ResearchRequest request;
    request.mutable_history_usage()->set_id(id);
    return io.submit<Json>(
        [state = std::make_shared<Impl>(io, endpoint), request = std::move(request), deadline](
            std::stop_token stop) {
            return state->read(request, stop, deadline);
        });
}

std::future<Json> ResearchClient::project(const research::proto::ProjectRequest& query)
{
    research::proto::ResearchRequest request;
    *request.mutable_project() = query;
    return impl_->io.submit<Json>(
        [state = impl_, request = std::move(request)](std::stop_token stop) mutable {
            return state->read(std::move(request), stop);
        });
}

std::future<Json> ResearchClient::assessment(const research::proto::BatchAssessmentRequest& query)
{
    research::proto::ResearchRequest request;
    *request.mutable_assessment() = query;
    return impl_->io.submit<Json>(
        [state = impl_, request = std::move(request)](std::stop_token stop) mutable {
            return state->read(std::move(request), stop);
        });
}

std::future<Json> ResearchClient::batch(const research::proto::BacktestBatchRequest& query)
{
    research::proto::ResearchRequest request;
    *request.mutable_batch() = query;
    return impl_->io.submit<Json>(
        [state = impl_, request = std::move(request)](std::stop_token stop) mutable {
            return state->read(std::move(request), stop);
        });
}

std::future<Json> ResearchClient::model(const research::proto::ModelRequest& query)
{
    research::proto::ResearchRequest request;
    *request.mutable_model() = query;
    return impl_->io.submit<Json>(
        [state = impl_, request = std::move(request)](std::stop_token stop) mutable {
            return state->read(std::move(request), stop);
        });
}

std::future<void>
ResearchClient::submit(const std::string& id, const research::proto::ModelTrainingRequest& input)
{
    research::proto::ResearchRequest request;
    request.mutable_submit()->set_id(id);
    *request.mutable_submit()->mutable_model_training() = input;
    return impl_->mutate(std::move(request));
}

std::future<void> ResearchClient::submit(
    const std::string& id,
    const research::proto::ModelTimeTrainingRequest& input)
{
    research::proto::ResearchRequest request;
    request.mutable_submit()->set_id(id);
    *request.mutable_submit()->mutable_model_time() = input;
    return impl_->mutate(std::move(request));
}

std::future<Json> ResearchClient::logic(const research::proto::LogicRequest& query)
{
    research::proto::ResearchRequest request;
    *request.mutable_logic() = query;
    return impl_->io.submit<Json>(
        [state = impl_, request = std::move(request)](std::stop_token stop) mutable {
            return state->read(std::move(request), stop);
        });
}

std::future<void>
ResearchClient::submit(const std::string& id, const research::proto::LogicTrialRequest& input)
{
    research::proto::ResearchRequest request;
    request.mutable_submit()->set_id(id);
    *request.mutable_submit()->mutable_logic_trial() = input;
    return impl_->mutate(std::move(request));
}

std::future<Json> ResearchClient::strategy(const research::proto::StrategyRequest& query)
{
    research::proto::ResearchRequest request;
    *request.mutable_strategy() = query;
    return impl_->io.submit<Json>(
        [state = impl_, request = std::move(request)](std::stop_token stop) mutable {
            return state->read(std::move(request), stop);
        });
}

std::future<Json> ResearchClient::variable(const research::proto::VariableRequest& query)
{
    research::proto::ResearchRequest request;
    *request.mutable_variable() = query;
    return impl_->io.submit<Json>(
        [state = impl_, request = std::move(request)](std::stop_token stop) mutable {
            return state->read(std::move(request), stop);
        });
}

std::future<Json> ResearchClient::project_tasks(const research::proto::ProjectTaskQuery& query)
{
    research::proto::ResearchRequest request;
    *request.mutable_project_tasks() = query;
    return impl_->io.submit<Json>(
        [state = impl_, request = std::move(request)](std::stop_token stop) mutable {
            return state->read(std::move(request), stop);
        });
}

std::future<Json> ResearchClient::resources(const resource::proto::CatalogueQuery& query)
{
    research::proto::ResearchRequest request;
    *request.mutable_resources() = query;
    return impl_->io.submit<Json>(
        [state = impl_, request = std::move(request)](std::stop_token stop) mutable {
            return state->read(std::move(request), stop);
        });
}

std::future<void> ResearchClient::action(const std::string& id, const std::string& action)
{
    research::proto::ResearchRequest request;
    if (action == "cancel")
        request.mutable_cancel()->set_id(id);
    else if (action == "retry")
        request.mutable_retry()->set_id(id);
    else
        throw std::invalid_argument("unsupported task action");
    return impl_->mutate(std::move(request));
}

std::future<Json> ResearchClient::result(const std::string& id)
{
    research::proto::ResearchRequest request;
    request.mutable_result()->set_id(id);
    return impl_->io.submit<Json>(
        [state = impl_, request = std::move(request)](std::stop_token stop) {
            return state->read(request, stop);
        });
}

} // namespace asterion::terminal
