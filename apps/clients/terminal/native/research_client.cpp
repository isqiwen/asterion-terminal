#include "research_client.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <stdexcept>
namespace asterion::terminal {
using namespace std::chrono_literals;
namespace wire = research::v1;
struct ResearchClient::Impl {
  ServiceEndpoint endpoint;
  const std::string connection_id = unique_process_id();
  std::mutex commands;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::jthread poller;
  Json tasks = Json::array();
  bool online = false;
  std::string error;
  explicit Impl(ServiceEndpoint value) : endpoint(std::move(value)) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
      try {
        refresh();
        break;
      } catch (const Error&) {
        if (std::chrono::steady_clock::now() > deadline)
          throw;
        std::this_thread::sleep_for(50ms);
      }
    }
    poller = std::jthread([this](std::stop_token stop) {
      for (;;) {
        {
          std::unique_lock lock(mutex);
          if (wake.wait_for(lock, 1s, [&] { return stop.stop_requested(); }))
            break;
        }
        try {
          std::lock_guard lock(commands);
          refresh();
        } catch (const std::exception& e) {
          std::lock_guard lock(mutex);
          online = false;
          error = e.what();
        }
      }
    });
  }
  ~Impl() {
    poller.request_stop();
    wake.notify_all();
    if (poller.joinable())
      poller.join();
  }
  wire::TaskResponse call(wire::TaskRequest request) {
    request.set_version(1);
    request.set_service_id(endpoint.session);
    request.set_correlation_id(unique_process_id());
    auto exchange = [&](auto channel) {
      channel.send(request.SerializeAsString(), 5s);
      return channel.receive(5s);
    };
    const auto raw =
        endpoint.endpoint.empty()
            ? exchange(ipc::TlsChannel::connect(endpoint.host, endpoint.port, endpoint.tls, 5s))
            : exchange(ipc::Channel::connect(endpoint.endpoint, 5s));
    wire::TaskResponse response;
    if (!response.ParseFromString(raw))
      throw Error(ErrorCode::unavailable, "invalid research response");
    protocol::validate_message(response);
    if (response.version() != 1 || response.service_id() != endpoint.session ||
        response.correlation_id() != request.correlation_id())
      throw Error(ErrorCode::unavailable, "research response identity mismatch");
    if (response.has_error())
      throw_remote_error(response.error().code(), response.error().message());
    if (request.has_daily_page()    ? !response.has_daily_page()
        : request.has_minute_page() ? !response.has_minute_page()
        : request.has_list()        ? !response.has_tasks()
        : request.has_result()
            ? (!response.has_backtest() && !response.has_factor() && !response.has_publication() &&
               !response.has_calendar_publication() && !response.has_minutes() &&
               !response.has_daily() && !response.has_daily_factor())
            : !response.has_task())
      throw Error(ErrorCode::unavailable, "unexpected research response");
    return response;
  }
  void refresh() {
    wire::TaskRequest request;
    request.mutable_list();
    const auto response = call(request);
    Json values = Json::array();
    for (const auto& task : response.tasks().tasks())
      values.push_back(protocol::decode_task(task));
    std::lock_guard lock(mutex);
    tasks = std::move(values);
    online = true;
    error.clear();
  }
};
ResearchClient::ResearchClient(ServiceEndpoint endpoint)
    : impl_(std::make_unique<Impl>(std::move(endpoint))) {}
ResearchClient::~ResearchClient() = default;
Json ResearchClient::status() const {
  std::lock_guard lock(impl_->mutex);
  return {{"connection_id", impl_->connection_id},
          {"service", impl_->endpoint.session},
          {"host", impl_->endpoint.endpoint.empty() ? impl_->endpoint.host : "localhost"},
          {"remote", impl_->endpoint.endpoint.empty()},
          {"online", impl_->online},
          {"error", impl_->error},
          {"tasks", impl_->tasks}};
}
void ResearchClient::submit(const std::string& id, const wire::BacktestInput& input) {
  std::lock_guard lock(impl_->commands);
  wire::TaskRequest request;
  request.mutable_submit()->set_id(id);
  *request.mutable_submit()->mutable_input() = input;
  impl_->call(request);
  impl_->refresh();
}
void ResearchClient::submit(const std::string& id, const wire::FactorInput& input) {
  std::lock_guard lock(impl_->commands);
  wire::TaskRequest request;
  request.mutable_submit()->set_id(id);
  *request.mutable_submit()->mutable_factor() = input;
  impl_->call(request);
  impl_->refresh();
}
void ResearchClient::submit(const std::string& id, const wire::DailyFactorRequest& input) {
  std::lock_guard lock(impl_->commands);
  wire::TaskRequest request;
  request.mutable_submit()->set_id(id);
  *request.mutable_submit()->mutable_daily_factor() = input;
  impl_->call(request);
  impl_->refresh();
}
void ResearchClient::submit(const std::string& id, const data::v1::CsvSnapshot& input) {
  std::lock_guard lock(impl_->commands);
  wire::TaskRequest request;
  request.mutable_submit()->set_id(id);
  *request.mutable_submit()->mutable_data() = input;
  impl_->call(request);
  impl_->refresh();
}
void ResearchClient::submit(const std::string& id, const data::v1::CalendarCsvSnapshot& input) {
  std::lock_guard lock(impl_->commands);
  research::v1::TaskRequest request;
  request.mutable_submit()->set_id(id);
  *request.mutable_submit()->mutable_calendar() = input;
  impl_->call(request);
  impl_->refresh();
}
void ResearchClient::submit(const std::string& id, const data::v1::MinuteDownload& input,
                            const std::string& token) {
  std::lock_guard lock(impl_->commands);
  if (impl_->endpoint.endpoint.empty())
    throw std::invalid_argument("Tushare downloads currently require a local research service");
  wire::TaskRequest request;
  auto* submit = request.mutable_submit();
  submit->set_id(id);
  *submit->mutable_minutes() = input;
  submit->set_provider_token(token);
  impl_->call(request);
  impl_->refresh();
}
void ResearchClient::submit(const std::string& id, const data::v1::DailyDownload& input,
                            const std::string& token) {
  std::lock_guard lock(impl_->commands);
  if (impl_->endpoint.endpoint.empty())
    throw std::invalid_argument("Tushare downloads currently require a local research service");
  wire::TaskRequest request;
  auto* submit = request.mutable_submit();
  submit->set_id(id);
  *submit->mutable_daily() = input;
  submit->set_provider_token(token);
  impl_->call(request);
  impl_->refresh();
}
void ResearchClient::action(const std::string& id, const std::string& action) {
  std::lock_guard lock(impl_->commands);
  wire::TaskRequest request;
  if (action == "cancel")
    request.mutable_cancel()->set_id(id);
  else if (action == "retry")
    request.mutable_retry()->set_id(id);
  else
    throw std::invalid_argument("unsupported research action");
  impl_->call(request);
  impl_->refresh();
}
Json ResearchClient::result(const std::string& id) {
  std::lock_guard lock(impl_->commands);
  wire::TaskRequest request;
  request.mutable_result()->set_id(id);
  const auto response = impl_->call(request);
  return protocol::decode_task_result(response, id);
}
Json ResearchClient::minute_page(const data::v1::MinutePageQuery& query) {
  wire::TaskRequest request;
  *request.mutable_minute_page() = query;
  const auto response = impl_->call(request);
  const auto& page = response.minute_page();
  if (page.task_id() != query.task_id() || page.offset() != query.offset() ||
      page.limit() != query.limit() || (query.begin_ns() && page.begin_ns() != query.begin_ns()) ||
      (query.end_ns() && page.end_ns() != query.end_ns()))
    throw Error(ErrorCode::unavailable, "minute dataset page identity mismatch");
  return protocol::decode_minute_page(page);
}
Json ResearchClient::daily_page(const data::v1::DailyPageQuery& query) {
  wire::TaskRequest request;
  *request.mutable_daily_page() = query;
  const auto response = impl_->call(request);
  const auto& page = response.daily_page();
  if (page.task_id() != query.task_id() || page.offset() != query.offset() ||
      page.limit() != query.limit() || page.period() != query.period() ||
      (!query.begin_day().empty() && page.begin_day() != query.begin_day()) ||
      (!query.end_day().empty() && page.end_day() != query.end_day()))
    throw Error(ErrorCode::unavailable, "daily dataset page identity mismatch");
  return protocol::decode_daily_page(page);
}
} // namespace asterion::terminal
