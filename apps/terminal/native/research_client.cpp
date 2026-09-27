#include "research_client.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace asterion::terminal {
using namespace std::chrono_literals;
namespace wire = research::v1;
struct ResearchClient::Impl {
  ServiceEndpoint endpoint;
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
      } catch (const Error &) {
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
        } catch (const std::exception &e) {
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
            ? exchange(ipc::TlsChannel::connect(endpoint.host, endpoint.port,
                                                endpoint.tls, 5s))
            : exchange(ipc::Channel::connect(endpoint.endpoint, 5s));
    wire::TaskResponse response;
    if (!response.ParseFromString(raw))
      throw Error(ErrorCode::unavailable, "invalid research response");
    protocol::validate_message(response);
    if (response.version() != 1 || response.service_id() != endpoint.session ||
        response.correlation_id() != request.correlation_id())
      throw Error(ErrorCode::unavailable,
                  "research response identity mismatch");
    if (response.has_error())
      throw std::invalid_argument(response.error().message());
    if (request.has_list() ? !response.has_tasks()
        : request.has_result()
            ? (!response.has_backtest() && !response.has_factor() && !response.has_publication() && !response.has_calendar_publication())
            : !response.has_task())
      throw Error(ErrorCode::unavailable, "unexpected research response");
    return response;
  }
  void refresh() {
    wire::TaskRequest request;
    request.mutable_list();
    const auto response = call(request);
    Json values = Json::array();
    for (const auto &task : response.tasks().tasks())
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
  return {{"service", impl_->endpoint.session},
          {"host", impl_->endpoint.endpoint.empty() ? impl_->endpoint.host
                                                    : "localhost"},
          {"remote", impl_->endpoint.endpoint.empty()},
          {"online", impl_->online},
          {"error", impl_->error},
          {"tasks", impl_->tasks}};
}
void ResearchClient::submit(const std::string &id,
                            const wire::BacktestInput &input) {
  std::lock_guard lock(impl_->commands);
  wire::TaskRequest request;
  request.mutable_submit()->set_id(id);
  *request.mutable_submit()->mutable_input() = input;
  impl_->call(request);
  impl_->refresh();
}
void ResearchClient::submit(const std::string &id,
                            const wire::FactorInput &input) {
  std::lock_guard lock(impl_->commands);
  wire::TaskRequest request;
  request.mutable_submit()->set_id(id);
  *request.mutable_submit()->mutable_factor() = input;
  impl_->call(request);
  impl_->refresh();
}
void ResearchClient::submit(const std::string &id, const data::v1::CsvSnapshot &input) {
  std::lock_guard lock(impl_->commands);
  wire::TaskRequest request; request.mutable_submit()->set_id(id);
  *request.mutable_submit()->mutable_data() = input;
  impl_->call(request); impl_->refresh();
}
void ResearchClient::submit(const std::string &id,const data::v1::CalendarCsvSnapshot &input) {
  std::lock_guard lock(impl_->commands);
  research::v1::TaskRequest request;request.mutable_submit()->set_id(id);*request.mutable_submit()->mutable_calendar()=input;
  impl_->call(request);impl_->refresh();
}
void ResearchClient::action(const std::string &id, const std::string &action) {
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
Json ResearchClient::result(const std::string &id) {
  std::lock_guard lock(impl_->commands);
  wire::TaskRequest request;
  request.mutable_result()->set_id(id);
  const auto response = impl_->call(request);
  return protocol::decode_task_result(response, id);
}
} // namespace asterion::terminal
