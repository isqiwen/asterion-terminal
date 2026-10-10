#include "data_client.hpp"
#include <asterion/protocol/data_client.hpp>
#include <asterion/protocol/backtest.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/trace.hpp>
#include <algorithm>
#include <array>
namespace asterion::terminal {
using namespace std::chrono_literals;
namespace wire = data::v1;
struct DataClient::Impl : std::enable_shared_from_this<Impl> {
  ServiceIo& io;
  const ServiceEndpoint endpoint;
  const std::string connection_id = unique_process_id();
  enum class Channel { request, observation };
  std::array<std::unique_ptr<ipc::RpcClient>, 2> transports;
  std::stop_source lifetime;
  Json sources = Json::array();
  std::vector<data::v1::HistorySource> typed_sources;
  bool online = false;
  std::string error;
  Json status() const {
    return Json{{"connection_id", connection_id},
                {"service", endpoint.session},
                {"port", endpoint.port},
                {"host", endpoint.endpoint.empty() ? endpoint.host : "localhost"},
                {"remote", endpoint.endpoint.empty()},
                {"online", online},
                {"error", error},
                {"sources", sources}};
  }
  Impl(ServiceIo& owner, ServiceEndpoint value) : io(owner), endpoint(std::move(value)) {
    validate_id(endpoint.session);
    // Prepare TLS files on the creating management operation before handoff to I/O.
    constexpr std::array<std::size_t, 2> slots{8, 1};
    for (std::size_t i = 0; i < transports.size(); ++i) {
      auto budget = io.payload_budget(i == static_cast<std::size_t>(Channel::request)
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
  PolledTask<Payload> exchange(std::string request, std::chrono::milliseconds timeout,
                               std::stop_token stop, std::chrono::steady_clock::time_point deadline,
                               Channel lane = Channel::request) {
    auto& transport = transports[static_cast<std::size_t>(lane)];
    if (stopped(stop))
      throw Error(ErrorCode::cancelled, "data connection closed");
    auto reply = transport->request(std::move(request), timeout, deadline);
    while (reply.wait_for(0ms) != std::future_status::ready) {
      if (stopped(stop))
        transport.reset();
      else
        transport->poll();
      co_await std::suspend_always{};
    }
    co_return reply.get();
  }
  void identify(wire::DataRequest& request) const {
    request.set_version(1);
    request.set_service_id(endpoint.session);
    request.set_correlation_id(next_correlation_id());
  }
  template <class T, class Decode>
  std::future<T> invoke(wire::DataRequest request, Decode decode, bool large_reply = true,
                        std::chrono::milliseconds timeout = 60s,
                        std::chrono::steady_clock::time_point deadline =
                            std::chrono::steady_clock::time_point::max()) {
    deadline = std::min(deadline, std::chrono::steady_clock::now() + timeout);
    return io.submit<T>([state = shared_from_this(), request = std::move(request),
                         decode = std::move(decode), large_reply, timeout,
                         deadline](std::stop_token stop) mutable -> PolledTask<T> {
      state->identify(request);
      std::string encoded;
      if (request.has_save_dataset())
        encoded = co_await state->io.read<std::string>([&] {
          protocol::validate_named_dataset(request.save_dataset());
          return request.SerializeAsString();
        });
      else
        encoded = request.SerializeAsString();
      auto bytes = co_await state->exchange(std::move(encoded), timeout, stop, deadline);
      auto process = [&]() -> T {
        auto response = protocol::decode_data_response(request, *bytes);
        return decode(request, response);
      };
      if (large_reply)
        co_return co_await state->io.read<T>(process);
      co_return process();
    });
  }
  PolledTask<void> refresh(std::stop_token stop) {
    wire::DataRequest request;
    request.mutable_sources();
    identify(request);
    const auto response = protocol::decode_data_response(
        request, *(co_await exchange(request.SerializeAsString(), 5s, stop,
                                     std::chrono::steady_clock::now() + 5s, Channel::observation)));
    Json declared = Json::array();
    for (const auto& source : response.sources().sources()) {
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
    sources = std::move(declared);
    typed_sources.assign(response.sources().sources().begin(), response.sources().sources().end());
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
DataClient::DataClient(ServiceIo& io, ServiceEndpoint endpoint)
    : impl_(std::make_shared<Impl>(io, std::move(endpoint))) {}
std::future<std::shared_ptr<DataClient>> DataClient::open(ServiceIo& io, ServiceEndpoint endpoint) {
  return io.submit<std::shared_ptr<DataClient>>(
      [&io, endpoint = std::move(endpoint)](
          std::stop_token stop) mutable -> PolledTask<std::shared_ptr<DataClient>> {
        auto client = co_await io.admin<std::shared_ptr<DataClient>>(
            [&] { return std::shared_ptr<DataClient>(new DataClient(io, std::move(endpoint))); });
        co_await client->impl_->initialize(stop);
        (void)client->impl_->io.submit<void>(
            [state = client->impl_](std::stop_token stop) { return state->monitor(stop); },
            ServiceIo::Lane::observation);
        co_return client;
      });
}
DataClient::~DataClient() {
  impl_->lifetime.request_stop();
}
std::future<std::string>
DataClient::authorize_download(const wire::DownloadAuthorizationRequest& input) {
  wire::DataRequest request;
  *request.mutable_authorize_download() = input;
  return impl_->invoke<std::string>(
      std::move(request),
      [](const wire::DataRequest& request, wire::DataResponse& response) -> std::string {
        const auto& input = request.authorize_download();
        const auto& authorization = response.download_authorization();
        if (authorization.version() != 2 || authorization.data_instance() != request.service_id() ||
            authorization.task_instance() != input.task_instance() ||
            authorization.task_id() != input.task_id())
          throw Error(ErrorCode::unavailable, "download authorization response identity mismatch");
        return authorization.id();
      },
      false, 120s);
}
std::future<void>
DataClient::configure_download_budget(const wire::DownloadBudgetConfiguration& input) {
  wire::DataRequest request;
  *request.mutable_configure_download_budget() = input;
  return impl_->invoke<void>(
      std::move(request), [](const wire::DataRequest&, wire::DataResponse&) -> void {}, false,
      120s);
}
ServiceEndpoint DataClient::endpoint() const {
  return impl_->endpoint;
}
Json DataClient::owner_status() const {
  return impl_->status();
}
std::future<Json> DataClient::status() const {
  return impl_->io.submit<Json>(
      [state = impl_](std::stop_token) -> PolledTask<Json> { co_return state->status(); });
}
std::future<data::v1::HistorySource> DataClient::source(const std::string& id) const {
  return impl_->io.submit<data::v1::HistorySource>(
      [state = impl_, id](std::stop_token) -> PolledTask<data::v1::HistorySource> {
        for (const auto& source : state->typed_sources)
          if (source.id() == id)
            co_return source;
        throw std::invalid_argument("historical data source is unavailable");
      });
}
std::future<std::vector<data::v1::HistorySource>>
DataClient::provider_sources(const std::string& plugin_id) const {
  return impl_->io.submit<std::vector<data::v1::HistorySource>>(
      [state = impl_,
       plugin_id](std::stop_token) -> PolledTask<std::vector<data::v1::HistorySource>> {
        std::vector<data::v1::HistorySource> result;
        for (const auto& source : state->typed_sources)
          if (source.plugin_id() == plugin_id && source.has_connection())
            result.push_back(source);
        co_return result;
      });
}
std::future<data::v1::HistoryConnectionVerification>
DataClient::verify_connection(const std::string& source, const std::string& credential) {
  wire::DataRequest request;
  request.mutable_verify_connection()->set_source(source);
  request.mutable_verify_connection()->set_credential(credential);
  return impl_->invoke<data::v1::HistoryConnectionVerification>(
      std::move(request),
      [](const wire::DataRequest&,
         wire::DataResponse& response) -> data::v1::HistoryConnectionVerification {
        return std::move(*response.mutable_connection_verification());
      },
      true, 90s);
}
std::future<backtest::v1::DominantSeriesPreview>
DataClient::dominant_series(const std::vector<data::v1::BarDatasetRequest>& months) {
  wire::DataRequest request;
  for (const auto& month : months)
    *request.mutable_dominant_series()->add_months() = month;
  return impl_->invoke<backtest::v1::DominantSeriesPreview>(
      std::move(request),
      [](const wire::DataRequest& request,
         wire::DataResponse& response) -> backtest::v1::DominantSeriesPreview {
        const auto& months = request.dominant_series().months();
        auto& preview = *response.mutable_dominant_series();
        if (preview.months().empty() || preview.months_size() != preview.datasets_size() ||
            preview.schedule().rolls_size() != preview.months_size())
          throw Error(ErrorCode::unavailable, "bar dataset identity mismatch");
        for (int i = 0; i < preview.months_size(); ++i) {
          protocol::validate_bar_dataset(preview.datasets(i));
          if (preview.months(i) >= static_cast<unsigned>(months.size()) ||
              preview.schedule().rolls(i).contract() != unsigned(i) ||
              preview.datasets(i).contract().SerializeAsString() !=
                  months[preview.months(i)].contract().SerializeAsString())
            throw Error(ErrorCode::unavailable, "bar dataset identity mismatch");
        }
        return std::move(preview);
      });
}
std::future<data::v1::BarDataset>
DataClient::bar_dataset(const data::v1::BarDatasetRequest& query) {
  wire::DataRequest request;
  *request.mutable_bar_dataset() = query;
  return impl_->invoke<data::v1::BarDataset>(
      std::move(request),
      [](const wire::DataRequest& request, wire::DataResponse& response) -> data::v1::BarDataset {
        const auto& query = request.bar_dataset();
        auto& dataset = *response.mutable_bar_dataset();
        protocol::validate_bar_dataset(dataset);
        if (!std::ranges::equal(dataset.source_dataset_ids(), query.source_dataset_ids()) ||
            !std::ranges::equal(dataset.settlement_dataset_ids(), query.settlement_dataset_ids()) ||
            dataset.contract().SerializeAsString() != query.contract().SerializeAsString())
          throw Error(ErrorCode::unavailable, "bar dataset identity mismatch");
        return std::move(dataset);
      });
}
std::future<data::v1::HistoryUpdatePlan>
DataClient::history_update_plan(const data::v1::HistoryUpdateQuery& query) {
  wire::DataRequest request;
  *request.mutable_update_plan() = query;
  return impl_->invoke<data::v1::HistoryUpdatePlan>(
      std::move(request),
      [](const wire::DataRequest& request,
         wire::DataResponse& response) -> data::v1::HistoryUpdatePlan {
        const auto& query = request.update_plan();
        auto& plan = *response.mutable_update_plan();
        (void)protocol::decode_history_update_plan(plan);
        if (plan.query().SerializeAsString() != query.SerializeAsString())
          throw Error(ErrorCode::unavailable, "history update plan changed; preview again");
        return std::move(plan);
      });
}
std::future<Json> DataClient::datasets(const data::v1::HistoryFilter& filter) {
  wire::DataRequest request;
  *request.mutable_datasets() = filter;
  return impl_->invoke<Json>(std::move(request),
                             [](const wire::DataRequest&, wire::DataResponse& response) -> Json {
                               Json items = Json::array();
                               for (const auto& row : response.datasets().items())
                                 items.push_back({{"id", row.id()},
                                                  {"contract_id", row.contract_id()},
                                                  {"source", row.source()},
                                                  {"revision", row.revision()},
                                                  {"begin", row.begin()},
                                                  {"end", row.end()},
                                                  {"interval_minutes", row.interval_minutes()},
                                                  {"rows", row.rows()}});
                               return items;
                             });
}
std::future<Json> DataClient::coverage(const data::v1::HistoryFilter& filter) {
  wire::DataRequest request;
  *request.mutable_coverage() = filter;
  return impl_->invoke<Json>(std::move(request),
                             [](const wire::DataRequest&, wire::DataResponse& response) -> Json {
                               Json items = Json::array();
                               for (const auto& row : response.coverage().items()) {
                                 Json days = Json::array();
                                 for (const auto& day : row.uncovered_days())
                                   days.push_back(day);
                                 items.push_back({{"minute_dataset_id", row.minute_dataset_id()},
                                                  {"daily_dataset_id", row.daily_dataset_id()},
                                                  {"minute_source", row.minute_source()},
                                                  {"daily_source", row.daily_source()},
                                                  {"interval_minutes", row.interval_minutes()},
                                                  {"contract_id", row.contract_id()},
                                                  {"minute_days", row.minute_days()},
                                                  {"minute_first", row.minute_first()},
                                                  {"minute_last", row.minute_last()},
                                                  {"daily_days", row.daily_days()},
                                                  {"daily_first", row.daily_first()},
                                                  {"daily_last", row.daily_last()},
                                                  {"uncovered", row.uncovered()},
                                                  {"uncovered_days", std::move(days)}});
                               }
                               return items;
                             });
}
std::future<std::vector<HistoryListing>> DataClient::catalog(const std::string& source,
                                                             const std::string& credential,
                                                             const std::string& venue,
                                                             const std::string& product) {
  wire::DataRequest request;
  auto* q = request.mutable_catalog();
  q->set_source(source);
  q->set_credential(credential);
  q->set_venue(venue);
  q->set_product(product);
  return impl_->invoke<std::vector<HistoryListing>>(
      std::move(request),
      [](const wire::DataRequest& request,
         wire::DataResponse& response) -> std::vector<HistoryListing> {
        const auto& source = request.catalog().source();
        if (!response.has_catalog() || response.catalog().source() != source)
          throw std::invalid_argument("historical catalog identity mismatch");
        std::vector<HistoryListing> result;
        for (const auto& row : response.catalog().items()) {
          HistoryListing item{HistoryIdentity::parse(row.contract_id()), row.name(),
                              row.list_date(), row.delist_date(), row.source_instrument()};
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
      },
      true, 90s);
}
std::future<Json> DataClient::minute_page(const data::v1::MinutePageQuery& query) {
  wire::DataRequest request;
  *request.mutable_minute_page() = query;
  return impl_->invoke<Json>(
      std::move(request),
      [](const wire::DataRequest& request, wire::DataResponse& response) -> Json {
        const auto& query = request.minute_page();
        const auto& page = response.minute_page();
        if (page.task_id() != query.dataset_id() || page.offset() != query.offset() ||
            page.limit() != query.limit() ||
            (query.begin_ns() && page.begin_ns() != query.begin_ns()) ||
            (query.end_ns() && page.end_ns() != query.end_ns()))
          throw Error(ErrorCode::unavailable, "minute dataset page identity mismatch");
        return protocol::decode_minute_page(page);
      });
}
std::future<Json> DataClient::daily_page(const data::v1::DailyPageQuery& query) {
  wire::DataRequest request;
  *request.mutable_daily_page() = query;
  return impl_->invoke<Json>(
      std::move(request),
      [](const wire::DataRequest& request, wire::DataResponse& response) -> Json {
        const auto& query = request.daily_page();
        const auto& page = response.daily_page();
        if (page.task_id() != query.dataset_id() || page.offset() != query.offset() ||
            page.limit() != query.limit() || page.period() != query.period() ||
            (!query.begin_day().empty() && page.begin_day() != query.begin_day()) ||
            (!query.end_day().empty() && page.end_day() != query.end_day()))
          throw Error(ErrorCode::unavailable, "daily dataset page identity mismatch");
        return protocol::decode_daily_page(page);
      });
}
std::future<Json> DataClient::history_usage(const std::string& id) {
  wire::DataRequest request;
  request.mutable_history_usage()->set_id(id);
  return impl_->invoke<Json>(
      std::move(request),
      [](const wire::DataRequest& request, wire::DataResponse& response) -> Json {
        const auto& id = request.history_usage().id();
        if (response.history_usage().dataset_id() != id)
          throw Error(ErrorCode::unavailable, "invalid historical usage response");
        return protocol::decode_history_usage(response.history_usage());
      });
}
std::future<Json>
DataClient::inspect_history_usage(ServiceIo& io, const ServiceEndpoint& endpoint,
                                  const std::string& id,
                                  std::chrono::steady_clock::time_point deadline) {
  auto state = std::make_shared<Impl>(io, endpoint);
  wire::DataRequest request;
  request.mutable_history_usage()->set_id(id);
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (remaining <= 0ms)
    throw Error(ErrorCode::unavailable, "data request timed out");
  return state->invoke<Json>(
      std::move(request),
      [](const wire::DataRequest& request, wire::DataResponse& response) {
        if (response.history_usage().dataset_id() != request.history_usage().id())
          throw Error(ErrorCode::unavailable, "invalid historical usage response");
        return protocol::decode_history_usage(response.history_usage());
      },
      true, remaining, deadline);
}
std::future<Json> DataClient::saved_datasets() {
  wire::DataRequest request;
  request.mutable_saved_datasets();
  return impl_->invoke<Json>(std::move(request),
                             [](const wire::DataRequest&, wire::DataResponse& response) -> Json {
                               Json items = Json::array();
                               for (const auto& item : response.saved_datasets().items())
                                 items.push_back(protocol::decode_named_dataset(item));
                               return items;
                             });
}
std::future<data::v1::NamedDataset> DataClient::saved_dataset(const std::string& id) {
  wire::DataRequest request;
  request.mutable_saved_dataset()->set_id(id);
  return impl_->invoke<data::v1::NamedDataset>(
      std::move(request),
      [](const wire::DataRequest& request, wire::DataResponse& response) -> data::v1::NamedDataset {
        auto& value = *response.mutable_saved_dataset();
        protocol::validate_named_dataset(value);
        if (value.id() != request.saved_dataset().id())
          throw std::invalid_argument("saved dataset revision mismatch");
        return std::move(value);
      });
}
std::future<void> DataClient::save_dataset(const data::v1::NamedDataset& value) {
  wire::DataRequest request;
  *request.mutable_save_dataset() = value;
  return impl_->invoke<void>(
      std::move(request),
      [](const wire::DataRequest& request, wire::DataResponse& response) -> void {
        const auto& value = request.save_dataset();
        if (response.saved_dataset().SerializeAsString() != value.SerializeAsString())
          throw std::invalid_argument("saved dataset revision mismatch");
      });
}
} // namespace asterion::terminal
