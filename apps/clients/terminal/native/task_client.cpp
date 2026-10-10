#include "task_client.hpp"
#include "performance.hpp"
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/kernel/process/child.hpp>
#include <optional>
#include <algorithm>
#include <array>
#include <map>
namespace asterion::terminal {
using namespace std::chrono_literals;
namespace wire = task::v1;
namespace {
Json figures(const Performance& value) {
  const auto optional = [](const std::optional<double>& statistic) {
    return statistic ? Json(*statistic) : Json(nullptr);
  };
  return {{"trading_days", value.trading_days},
          {"total_return", value.total_return},
          {"max_drawdown", value.max_drawdown},
          {"winning_days", value.winning_days},
          {"annual_return", optional(value.annual_return)},
          {"annual_volatility", optional(value.annual_volatility)},
          {"sharpe", optional(value.sharpe)},
          {"calmar", optional(value.calmar)}};
}
// How a backtest's fixed result performed, over all of its days and, for a
// comparison, before and from the holdout: the holdout starts from the equity
// the development days ended with. Derived on every read, never stored.
Json backtest_performance(const backtest::v1::BacktestInput& input,
                          const backtest::v1::BacktestResult& result) {
  std::vector<EquityDay> days;
  for (const auto& day : result.settlements())
    days.push_back({std::chrono::sys_days(parse_trading_date(day.trading_day())),
                    Decimal::from_raw(day.equity().units())});
  std::vector<Decimal> marks;
  // How many days, and how many marks up to their last settlement, come
  // before the holdout.
  std::size_t development_days = 0, development_marks = 0;
  int settled = 0;
  for (const auto& point : result.equity()) {
    marks.push_back(Decimal::from_raw(point.equity().units()));
    if (point.event() != backtest::v1::DAILY_SETTLEMENT)
      continue;
    if (result.settlements(settled).trading_day() < input.holdout_day()) {
      development_days = static_cast<std::size_t>(settled) + 1;
      development_marks = marks.size();
    }
    ++settled;
  }
  const auto deposit = Decimal::from_raw(input.paper().deposit().units());
  auto value = figures(performance(deposit, days, marks));
  value["development"] = nullptr;
  value["holdout"] = nullptr;
  if (!input.holdout_day().empty()) {
    const std::span all_days(days);
    const std::span all_marks(marks);
    value["development"] = figures(
        performance(deposit, all_days.first(development_days), all_marks.first(development_marks)));
    // An account that the development days emptied has no return to speak of.
    if (const auto start = days[development_days - 1].equity; start > Decimal{})
      value["holdout"] = figures(performance(start, all_days.subspan(development_days),
                                             all_marks.subspan(development_marks)));
  }
  return value;
}
// What a backtest's closed positions say, from its fixed fills. Derived on
// every read like the figures above.
Json backtest_trades(const backtest::v1::BacktestInput& input,
                     const backtest::v1::BacktestResult& result) {
  const auto& contracts = input.paper().contracts();
  std::vector<Decimal> multipliers;
  for (const auto& contract : contracts)
    multipliers.push_back(Decimal::from_raw(contract.dataset().contract().multiplier().units()));
  // A fill says which order it filled; the order says which way.
  std::map<std::string, bool> buys;
  for (const auto& order : result.account().orders())
    buys[order.id()] = order.side() == protocol::v1::BUY;
  std::vector<TradedFill> fills;
  for (const auto& fill : result.account().fills()) {
    const auto contract = std::ranges::find_if(contracts, [&](const auto& item) {
      return item.dataset().contract().venue() == fill.venue() &&
             item.dataset().contract().symbol() == fill.symbol();
    });
    fills.push_back({static_cast<std::size_t>(contract - contracts.begin()),
                     buys.at(fill.order_id()), Decimal::from_raw(fill.quantity().units()),
                     Decimal::from_raw(fill.price().units())});
  }
  const auto value = trades(fills, multipliers);
  const auto optional = [](const std::optional<double>& statistic) {
    return statistic ? Json(*statistic) : Json(nullptr);
  };
  return {{"count", value.count},
          {"winning", value.winning},
          {"losing", value.losing},
          {"average_win", optional(value.average_win)},
          {"average_loss", optional(value.average_loss)},
          {"payoff", optional(value.payoff)}};
}
wire::TaskResponse decode_task_response(const wire::TaskRequest& request, const std::string& raw) {
  wire::TaskResponse response;
  if (!response.ParseFromString(raw))
    throw Error(ErrorCode::unavailable, "invalid task response");
  protocol::validate_message(response);
  if (response.version() != 1 || response.service_id() != request.service_id() ||
      response.correlation_id() != request.correlation_id())
    throw Error(ErrorCode::unavailable, "task response identity mismatch");
  if (response.has_error())
    throw_remote_error(response.error().code(), response.error().message());
  if (request.has_history_usage() ? !response.has_history_usage()
      : request.has_list()        ? !response.has_tasks()
      : request.has_result()      ? (!response.has_backtest() && !response.has_factor() &&
                                     !response.has_minutes() && !response.has_daily())
                                  : !response.has_task())
    throw Error(ErrorCode::unavailable, "unexpected task response");
  return response;
}
Json decode_task_page(const wire::TaskRequest& request, const std::string& raw) {
  const auto response = decode_task_response(request, raw);
  const auto before = request.list().before_sequence();
  const auto& usage = response.tasks().capacity();
  if (!response.tasks().has_capacity() || !usage.active_limit() ||
      usage.active_used() > usage.active_limit() || usage.active_used() > usage.retained_tasks() ||
      usage.active_reserved() > 1 ||
      usage.active_reserved() > usage.active_limit() - usage.active_used())
    throw std::invalid_argument("invalid task capacity");
  Json storage{{"retained_tasks", usage.retained_tasks()},
               {"active_used", usage.active_used()},
               {"active_reserved", usage.active_reserved()},
               {"uncommitted", usage.uncommitted()},
               {"active_limit", usage.active_limit()}};
  const auto& list = response.tasks();
  if (list.tasks_size() > 200 ||
      list.active_tasks_size() != static_cast<int>(usage.active_used()) ||
      (list.next_before_sequence() &&
       (list.tasks().empty() ||
        list.next_before_sequence() != list.tasks(0).submission_sequence())))
    throw std::invalid_argument("invalid task page response");
  std::map<unsigned, Json> ordered;
  unsigned previous = 0;
  for (const auto& task : list.tasks()) {
    if (task.submission_sequence() <= previous || (before && task.submission_sequence() >= before))
      throw std::invalid_argument("invalid task page response");
    previous = task.submission_sequence();
    ordered.emplace(previous, protocol::decode_task(task));
  }
  for (const auto& task : list.active_tasks()) {
    if (task.state() != wire::QUEUED && task.state() != wire::RUNNING &&
        task.state() != wire::CANCEL_REQUESTED && task.state() != wire::PUBLISHING)
      throw std::invalid_argument("invalid task page response");
    auto value = protocol::decode_task(task);
    const auto [found, added] = ordered.emplace(task.submission_sequence(), value);
    if (!added && found->second != value)
      throw std::invalid_argument("invalid task page response");
  }
  Json values = Json::array();
  for (auto& [sequence, value] : ordered)
    values.push_back(std::move(value));
  return Json{{"capacity", std::move(storage)},
              {"before_sequence", before},
              {"next_before_sequence", list.next_before_sequence()},
              {"failed_count", list.failed_count()},
              {"interrupted_count", list.interrupted_count()},
              {"tasks", std::move(values)}};
}
} // namespace
struct TaskClient::Impl : std::enable_shared_from_this<Impl> {
  ServiceIo& io;
  const ServiceEndpoint endpoint;
  const std::string connection_id = unique_process_id();
  // Long reads cannot occupy the control or list-observation slots.
  enum class Lane { read, control, observation };
  std::array<std::unique_ptr<ipc::RpcClient>, 3> transports;
  std::stop_source lifetime;
  bool refreshing = false;
  std::shared_ptr<const Json> page = std::make_shared<const Json>(Json{{"capacity", nullptr},
                                                                       {"before_sequence", 0},
                                                                       {"next_before_sequence", 0},
                                                                       {"failed_count", 0},
                                                                       {"interrupted_count", 0},
                                                                       {"tasks", Json::array()}});
  bool online = false;
  std::string error;
  Read view() const {
    return {page, Json{{"connection_id", connection_id},
                       {"service", endpoint.session},
                       {"port", endpoint.port},
                       {"host", endpoint.endpoint.empty() ? endpoint.host : "localhost"},
                       {"remote", endpoint.endpoint.empty()},
                       {"online", online},
                       {"error", error}}};
  }
  Impl(ServiceIo& owner, ServiceEndpoint value) : io(owner), endpoint(std::move(value)) {
    validate_id(endpoint.session);
    // Prepare TLS files on the creating management operation before handoff to I/O.
    constexpr std::array<std::size_t, 3> slots{8, 4, 1};
    for (std::size_t i = 0; i < transports.size(); ++i) {
      auto budget = io.payload_budget(i == static_cast<std::size_t>(Lane::read)
                                          ? ServiceIo::PayloadLane::data
                                          : ServiceIo::PayloadLane::control);
      transports[i] =
          endpoint.endpoint.empty()
              ? std::make_unique<ipc::RpcClient>(io.reactor(), endpoint.host, endpoint.port,
                                                 endpoint.tls, slots[i], budget)
              : std::make_unique<ipc::RpcClient>(io.reactor(), endpoint.endpoint, slots[i], budget);
    }
  }
  bool stopped(std::stop_token stop) const {
    return stop.stop_requested() || lifetime.stop_requested();
  }
  PolledTask<Payload> exchange(
      std::string request, std::chrono::milliseconds timeout, Lane lane, std::stop_token stop,
      std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max(),
      std::chrono::milliseconds send_timeout = 5s) {
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
  void identify(wire::TaskRequest& request) const {
    request.set_version(1);
    request.set_service_id(endpoint.session);
    request.set_correlation_id(next_correlation_id());
  }
  PolledTask<Payload>
  request_bytes(wire::TaskRequest& request, std::stop_token stop,
                std::optional<std::chrono::steady_clock::time_point> deadline = {}) {
    identify(request);
    std::chrono::milliseconds timeout = request.has_result()
                                            ? protocol::task_verification_timeout + 5s
                                        : request.has_get() ? protocol::task_input_timeout + 5s
                                        : request.has_history_usage() || request.has_submit() ? 60s
                                                                                              : 5s;
    if (deadline) {
      timeout = std::min(timeout, std::chrono::duration_cast<std::chrono::milliseconds>(
                                      *deadline - std::chrono::steady_clock::now()));
      if (timeout <= 0ms)
        throw Error(ErrorCode::unavailable, "task reference inspection timed out");
    }
    const auto lane = request.has_result() || request.has_get() || request.has_history_usage()
                          ? Lane::read
                          : Lane::control;
    const auto send_timeout = request.has_submit() || request.has_history_usage() ? 30s : 5s;
    auto bytes = request.has_submit()
                     ? co_await io.read<std::string>([&] { return request.SerializeAsString(); })
                     : request.SerializeAsString();
    co_return co_await exchange(std::move(bytes), timeout, lane, stop,
                                deadline.value_or(std::chrono::steady_clock::time_point::max()),
                                send_timeout);
  }
  PolledTask<Json> read(wire::TaskRequest request, std::stop_token stop,
                        std::optional<std::chrono::steady_clock::time_point> deadline = {}) {
    auto raw = co_await request_bytes(request, stop, deadline);
    co_return co_await io.read<Json>([request = std::move(request), raw = std::move(raw)] {
      const auto response = decode_task_response(request, *raw);
      if (request.has_result()) {
        auto evidence = protocol::decode_task_result(response, request.result().id());
        if (response.has_backtest()) {
          evidence["performance"] =
              backtest_performance(response.result_task().input(), response.backtest());
          evidence["trades"] = backtest_trades(response.result_task().input(), response.backtest());
        }
        return evidence;
      }
      if (response.history_usage().dataset_id() != request.history_usage().id())
        throw Error(ErrorCode::unavailable, "invalid historical usage response");
      return protocol::decode_history_usage(response.history_usage());
    });
  }
  std::future<void> mutate(wire::TaskRequest request) {
    return io.submit<void>([state = shared_from_this(), request = std::move(request)](
                               std::stop_token stop) mutable -> PolledTask<void> {
      const auto raw = co_await state->request_bytes(request, stop);
      (void)decode_task_response(request, *raw);
      co_await state->refresh(stop,
                              request.has_submit() ? std::optional<unsigned>{0} : std::nullopt);
    });
  }
  PolledTask<void> refresh(std::stop_token stop, std::optional<unsigned> requested_page = {}) {
    co_await PollUntil{[&] { return !refreshing || stopped(stop); }};
    if (stopped(stop))
      throw Error(ErrorCode::cancelled, "task connection closed");
    refreshing = true;
    struct Release {
      bool& refreshing;
      ~Release() { refreshing = false; }
    } release{refreshing};
    wire::TaskRequest request;
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
  PolledTask<void> initialize(std::stop_token stop) {
    try {
      co_await refresh(stop);
    } catch (const Error& e) {
      error = e.what();
    }
  }
  PolledTask<void> monitor(std::stop_token stop) {
    while (!stopped(stop)) {
      const auto next = std::chrono::steady_clock::now() + 1s;
      co_await PollUntil{[&] { return stopped(stop) || std::chrono::steady_clock::now() >= next; }};
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
TaskClient::TaskClient(ServiceIo& io, ServiceEndpoint endpoint)
    : impl_(std::make_shared<Impl>(io, std::move(endpoint))) {}
std::future<std::shared_ptr<TaskClient>> TaskClient::open(ServiceIo& io, ServiceEndpoint endpoint) {
  return io.submit<std::shared_ptr<TaskClient>>(
      [&io, endpoint = std::move(endpoint)](
          std::stop_token stop) mutable -> PolledTask<std::shared_ptr<TaskClient>> {
        auto client = co_await io.admin<std::shared_ptr<TaskClient>>(
            [&] { return std::shared_ptr<TaskClient>(new TaskClient(io, std::move(endpoint))); });
        co_await client->impl_->initialize(stop);
        (void)client->impl_->io.submit<void>(
            [state = client->impl_](std::stop_token stop) { return state->monitor(stop); },
            ServiceIo::Lane::observation);
        co_return client;
      });
}
TaskClient::~TaskClient() {
  impl_->lifetime.request_stop();
}
Json TaskClient::Read::render() const {
  auto result = connection;
  result.update(*page);
  return result;
}
TaskClient::Read TaskClient::owner_view() const {
  return impl_->view();
}
std::future<Json> TaskClient::status() const {
  return impl_->io.submit<Json>([state = impl_](std::stop_token) -> PolledTask<Json> {
    auto view = state->view();
    co_return co_await state->io.read<Json>([view = std::move(view)] { return view.render(); },
                                            ServiceIo::ReadLane::response);
  });
}
std::future<void> TaskClient::page(unsigned before_sequence) {
  return impl_->io.submit<void>([state = impl_, before_sequence](std::stop_token stop) {
    return state->refresh(stop, before_sequence);
  });
}
std::future<void> TaskClient::submit(const std::string& id,
                                     const asterion::backtest::v1::BacktestRequest& input) {
  wire::TaskRequest request;
  request.mutable_submit()->set_id(id);
  *request.mutable_submit()->mutable_backtest() = input;
  return impl_->mutate(std::move(request));
}
std::future<void> TaskClient::submit(const std::string& id,
                                     const asterion::factor::v1::FactorRequest& input) {
  wire::TaskRequest request;
  request.mutable_submit()->set_id(id);
  *request.mutable_submit()->mutable_factor_request() = input;
  return impl_->mutate(std::move(request));
}
std::future<void> TaskClient::submit_download(const std::string& id,
                                              const std::string& authorization) {
  wire::TaskRequest request;
  request.mutable_submit()->set_id(id);
  request.mutable_submit()->set_download_authorization(authorization);
  return impl_->mutate(std::move(request));
}
ServiceEndpoint TaskClient::endpoint() const {
  return impl_->endpoint;
}
std::future<Json> TaskClient::history_usage(const std::string& id) {
  wire::TaskRequest request;
  request.mutable_history_usage()->set_id(id);
  return impl_->io.submit<Json>([state = impl_, request = std::move(request)](
                                    std::stop_token stop) { return state->read(request, stop); });
}
std::future<Json>
TaskClient::inspect_history_usage(ServiceIo& io, const ServiceEndpoint& endpoint,
                                  const std::string& id,
                                  std::chrono::steady_clock::time_point deadline) {
  if (endpoint.session.empty() ||
      (endpoint.endpoint.empty() && (endpoint.host.empty() || !endpoint.port)))
    throw std::invalid_argument("invalid historical service address");
  wire::TaskRequest request;
  request.mutable_history_usage()->set_id(id);
  return io.submit<Json>(
      [state = std::make_shared<Impl>(io, endpoint), request = std::move(request),
       deadline](std::stop_token stop) { return state->read(request, stop, deadline); });
}
std::future<void> TaskClient::action(const std::string& id, const std::string& action) {
  wire::TaskRequest request;
  if (action == "cancel")
    request.mutable_cancel()->set_id(id);
  else if (action == "retry")
    request.mutable_retry()->set_id(id);
  else
    throw std::invalid_argument("unsupported task action");
  return impl_->mutate(std::move(request));
}
std::future<Json> TaskClient::result(const std::string& id) {
  wire::TaskRequest request;
  request.mutable_result()->set_id(id);
  return impl_->io.submit<Json>([state = impl_, request = std::move(request)](
                                    std::stop_token stop) { return state->read(request, stop); });
}
} // namespace asterion::terminal
