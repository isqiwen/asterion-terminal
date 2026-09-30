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
  Json sources = Json::array();
  std::vector<data::v1::HistorySource> typed_sources;
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
      // Dataset resolution reads and verifies every stored segment; large
      // research datasets (and submissions that resolve one) take longer.
      const bool dataset = request.has_bar_dataset() ||
                           (request.has_submit() && (request.submit().has_backtest() ||
                                                     request.submit().has_factor_request()));
      channel.send(request.SerializeAsString(), dataset ? 30s : 5s);
      return channel.receive((request.has_history_catalog() || request.has_verify_connection())
                                 ? 90s
                             : dataset ? 60s
                                       : 5s);
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
    if (request.has_verify_connection()  ? !response.has_connection_verification()
        : request.has_history_catalog()  ? !response.has_history_catalog()
        : request.has_history_datasets() ? !response.has_history_datasets()
        : request.has_daily_page()       ? !response.has_daily_page()
        : request.has_minute_page()      ? !response.has_minute_page()
        : request.has_bar_dataset()      ? !response.has_bar_dataset()
        : request.has_list()             ? !response.has_tasks()
        : request.has_result()
            ? (!response.has_backtest() && !response.has_factor() && !response.has_minutes() &&
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
    Json declared = Json::array();
    for (const auto& source : response.tasks().sources()) {
      declared.push_back(
          {{"id", source.id()},
           {"name", source.name()},
           {"plugin_id", source.plugin_id()},
           {"normalization", source.normalization()},
           {"timezone", source.timezone()},
           {"timestamp_semantics", source.timestamp_semantics()},
           {"venues", std::vector<std::string>(source.venues().begin(), source.venues().end())},
           {"intervals",
            std::vector<uint32_t>(source.intervals().begin(), source.intervals().end())},
           {"max_requests_per_minute", source.max_requests_per_minute()},
           {"credential_required", source.credential_required()},
           {"connection",
            source.has_connection()
                ? Json{{"credential_label_en", source.connection().credential_label_en()},
                       {"credential_label_zh", source.connection().credential_label_zh()},
                       {"credential_required", source.connection().credential_required()},
                       {"credential_max_length", source.connection().credential_max_length()},
                       {"remember_allowed", source.connection().remember_allowed()},
                       {"requests_per_minute_default",
                        source.connection().requests_per_minute_default()},
                       {"requests_per_minute_max", source.connection().requests_per_minute_max()}}
                : Json(nullptr)}});
    }
    std::lock_guard lock(mutex);
    sources = std::move(declared);
    typed_sources.assign(response.tasks().sources().begin(), response.tasks().sources().end());
    tasks = std::move(values);
    online = true;
    error.clear();
  }
};
ResearchClient::ResearchClient(ServiceEndpoint endpoint)
    : impl_(std::make_unique<Impl>(std::move(endpoint))) {}
ResearchClient::~ResearchClient() = default;
data::v1::HistorySource ResearchClient::source(const std::string& id) const {
  std::lock_guard lock(impl_->mutex);
  for (const auto& source : impl_->typed_sources)
    if (source.id() == id)
      return source;
  throw std::invalid_argument("historical data source is unavailable");
}
data::v1::HistoryConnectionVerification
ResearchClient::verify_connection(const std::string& source, const std::string& credential) {
  wire::TaskRequest request;
  request.mutable_verify_connection()->set_source(source);
  request.mutable_verify_connection()->set_credential(credential);
  return impl_->call(request).connection_verification();
}
Json ResearchClient::status() const {
  std::lock_guard lock(impl_->mutex);
  return {{"connection_id", impl_->connection_id},
          {"service", impl_->endpoint.session},
          {"host", impl_->endpoint.endpoint.empty() ? impl_->endpoint.host : "localhost"},
          {"remote", impl_->endpoint.endpoint.empty()},
          {"online", impl_->online},
          {"error", impl_->error},
          {"sources", impl_->sources},
          {"tasks", impl_->tasks}};
}
Json ResearchClient::tasks() {
  {
    std::lock_guard lock(impl_->commands);
    impl_->refresh();
  }
  std::lock_guard lock(impl_->mutex);
  return impl_->tasks;
}
void ResearchClient::submit(const std::string& id, const wire::BacktestRequest& input) {
  std::lock_guard lock(impl_->commands);
  wire::TaskRequest request;
  request.mutable_submit()->set_id(id);
  *request.mutable_submit()->mutable_backtest() = input;
  impl_->call(request);
  impl_->refresh();
}
void ResearchClient::submit(const std::string& id, const wire::FactorRequest& input) {
  std::lock_guard lock(impl_->commands);
  wire::TaskRequest request;
  request.mutable_submit()->set_id(id);
  *request.mutable_submit()->mutable_factor_request() = input;
  impl_->call(request);
  impl_->refresh();
}
data::v1::BarDataset ResearchClient::bar_dataset(const data::v1::BarDatasetRequest& query) {
  wire::TaskRequest request;
  *request.mutable_bar_dataset() = query;
  const auto response = impl_->call(request);
  const auto& dataset = response.bar_dataset();
  protocol::validate_bar_dataset(dataset);
  if (dataset.source_task_id() != query.source_task_id() ||
      dataset.settlement_task_id() != query.settlement_task_id() ||
      dataset.contract().SerializeAsString() != query.contract().SerializeAsString())
    throw Error(ErrorCode::unavailable, "bar dataset identity mismatch");
  return dataset;
}
void ResearchClient::submit(const std::string& id, const wire::DailyFactorRequest& input) {
  std::lock_guard lock(impl_->commands);
  wire::TaskRequest request;
  request.mutable_submit()->set_id(id);
  *request.mutable_submit()->mutable_daily_factor() = input;
  impl_->call(request);
  impl_->refresh();
}
void ResearchClient::submit(const std::string& id, const data::v1::MinuteDownload& input,
                            const std::string& token) {
  std::lock_guard lock(impl_->commands);
  if (impl_->endpoint.endpoint.empty())
    throw std::invalid_argument("Historical downloads currently require a local research service");
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
    throw std::invalid_argument("Historical downloads currently require a local research service");
  wire::TaskRequest request;
  auto* submit = request.mutable_submit();
  submit->set_id(id);
  *submit->mutable_daily() = input;
  submit->set_provider_token(token);
  impl_->call(request);
  impl_->refresh();
}
Json ResearchClient::datasets(const data::v1::HistoryFilter& filter) {
  wire::TaskRequest request;
  *request.mutable_history_datasets() = filter;
  const auto response = impl_->call(request);
  Json items = Json::array();
  for (const auto& row : response.history_datasets().items())
    items.push_back({{"id", row.id()},
                     {"contract_id", row.contract_id()},
                     {"source", row.source()},
                     {"revision", row.revision()},
                     {"begin", row.begin()},
                     {"end", row.end()},
                     {"interval_minutes", row.interval_minutes()},
                     {"rows", row.rows()}});
  return items;
}
std::vector<HistoryListing> ResearchClient::catalog(const std::string& source,
                                                    const std::string& credential,
                                                    const std::string& venue,
                                                    const std::string& product) {
  wire::TaskRequest request;
  auto* q = request.mutable_history_catalog();
  q->set_source(source);
  q->set_credential(credential);
  q->set_venue(venue);
  q->set_product(product);
  const auto response = impl_->call(request);
  if (!response.has_history_catalog() || response.history_catalog().source() != source)
    throw std::invalid_argument("historical catalog identity mismatch");
  std::vector<HistoryListing> result;
  for (const auto& row : response.history_catalog().items()) {
    HistoryListing item{HistoryIdentity::parse(row.contract_id()), row.name(), row.list_date(),
                        row.delist_date(), row.source_instrument()};
    if (row.has_multiplier())
      item.multiplier = Decimal::from_raw(row.multiplier().units());
    if (row.has_per_unit())
      item.per_unit = Decimal::from_raw(row.per_unit().units());
    if (row.has_trade_unit())
      item.trade_unit = row.trade_unit();
    if (row.has_quote_unit())
      item.quote_unit = row.quote_unit();
    result.push_back(std::move(item));
  }
  return result;
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
  if (page.task_id() != (query.dataset_id().empty() ? query.task_id() : query.dataset_id()) ||
      page.offset() != query.offset() || page.limit() != query.limit() ||
      (query.begin_ns() && page.begin_ns() != query.begin_ns()) ||
      (query.end_ns() && page.end_ns() != query.end_ns()))
    throw Error(ErrorCode::unavailable, "minute dataset page identity mismatch");
  return protocol::decode_minute_page(page);
}
Json ResearchClient::daily_page(const data::v1::DailyPageQuery& query) {
  wire::TaskRequest request;
  *request.mutable_daily_page() = query;
  const auto response = impl_->call(request);
  const auto& page = response.daily_page();
  if (page.task_id() != (query.dataset_id().empty() ? query.task_id() : query.dataset_id()) ||
      page.offset() != query.offset() || page.limit() != query.limit() ||
      page.period() != query.period() ||
      (!query.begin_day().empty() && page.begin_day() != query.begin_day()) ||
      (!query.end_day().empty() && page.end_day() != query.end_day()))
    throw Error(ErrorCode::unavailable, "daily dataset page identity mismatch");
  return protocol::decode_daily_page(page);
}
} // namespace asterion::terminal
