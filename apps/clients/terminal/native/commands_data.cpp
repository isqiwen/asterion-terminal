#include "application_impl.hpp"
#include <asterion/protocol/factor.hpp>
#include <asterion/domain/history_identity.hpp>

#include <stdexcept>
#include <algorithm>

namespace asterion::terminal {
namespace {
DatasetSelection resolved_selection(const data::v1::BarDatasetRequest& request,
                                    data::v1::BarDataset dataset) {
  const auto& bars = dataset.bars();
  const auto& first = bars.Get(0);
  const auto& last = bars.Get(bars.size() - 1);
  json uncovered = json::array();
  for (const auto& day : dataset.uncovered_days())
    uncovered.push_back(day);
  json summary = protocol::decode_bar_dataset_request(request);
  summary.update({{"venue", dataset.contract().venue()},
                  {"symbol", dataset.contract().symbol()},
                  {"revision", dataset.revision()},
                  {"source", dataset.source()},
                  {"interval_minutes", dataset.interval_minutes()},
                  {"count", bars.size()},
                  {"days", dataset.days_size()},
                  {"first_day", first.trading_day()},
                  {"last_day", last.trading_day()},
                  {"first_timestamp_ns", std::to_string(first.timestamp_ns())},
                  {"last_timestamp_ns", std::to_string(last.timestamp_ns())},
                  {"last_close", Decimal::from_raw(last.close().units()).str()},
                  {"uncovered_days", uncovered}});
  return DatasetSelection{request, std::move(dataset), std::move(summary)};
}
// The request for one archived contract: its identity comes from the
// archive, the units a data source does not provide from the user.
data::v1::BarDatasetRequest contract_request(const Json& archive, const json& sources,
                                             const json& settlements, const json& p) {
  if (!sources.is_array() || sources.empty())
    throw std::invalid_argument("dataset requires 1..32 versions per source role");
  std::string key;
  for (const auto& item : archive)
    if (item.at("id") == sources.at(0))
      key = item.at("contract_id").get<std::string>();
  if (key.empty())
    throw std::invalid_argument("historical dataset is unavailable");
  const auto identity = HistoryIdentity::parse(key);
  const auto id = identity.exchange_id();
  auto product = id.symbol.substr(0, identity.product.size());
  return protocol::encode_bar_dataset_request({{"source_dataset_ids", sources},
                                               {"settlement_dataset_ids", settlements},
                                               {"begin_day", p.at("begin_day")},
                                               {"end_day", p.at("end_day")},
                                               {"contract",
                                                {{"venue", id.venue},
                                                 {"symbol", id.symbol},
                                                 {"currency", "CNY"},
                                                 {"price_increment", p.at("price_increment")},
                                                 {"quantity_increment", "1"},
                                                 {"multiplier", p.at("multiplier")},
                                                 {"product", std::move(product)},
                                                 {"delivery_month", identity.delivery_month}}}});
}
} // namespace
data::v1::MinutePageQuery minute_page_query(const json& p) {
  fields(p, {"id", "offset", "limit", "start", "end", "include_macd"});
  if (!p.at("include_macd").is_boolean())
    throw std::invalid_argument("invalid minute dataset page query");
  if (!p.at("offset").is_number_integer() || p.at("offset") < 0 || p.at("offset") > 60000000 ||
      !p.at("limit").is_number_integer() || p.at("limit") < 1 || p.at("limit") > 200)
    throw std::invalid_argument("invalid minute dataset page query");
  data::v1::MinutePageQuery query;
  query.set_dataset_id(text(p, "id"));
  query.set_include_macd(p.at("include_macd").get<bool>());
  query.set_offset(p.at("offset").get<std::uint64_t>());
  query.set_limit(p.at("limit").get<unsigned>());
  if (!p.at("start").is_string() || !p.at("end").is_string())
    throw std::invalid_argument("invalid minute dataset page query");
  if (!p.at("start").get_ref<const std::string&>().empty())
    query.set_begin_ns(parse_shanghai_time(text(p, "start")));
  if (!p.at("end").get_ref<const std::string&>().empty())
    query.set_end_ns(parse_shanghai_time(text(p, "end")));
  return query;
}

data::v1::DailyPageQuery daily_page_query(const json& p) {
  fields(p, {"id", "offset", "limit", "start", "end", "include_macd", "period"});
  if (!p.at("include_macd").is_boolean() || !p.at("offset").is_number_integer() ||
      p.at("offset") < 0 || p.at("offset") > 20 * 366 + 1 || !p.at("limit").is_number_integer() ||
      p.at("limit") < 1 || p.at("limit") > 200 || !p.at("start").is_string() ||
      !p.at("end").is_string())
    throw std::invalid_argument("invalid daily dataset page query");
  data::v1::DailyPageQuery query;
  if (p.contains("period") && !p.at("period").is_string())
    throw std::invalid_argument("invalid daily dataset page query");
  const auto period = text(p, "period");
  if (period == "day")
    query.set_period(data::v1::DAY);
  else if (period == "week")
    query.set_period(data::v1::WEEK);
  else if (period == "month")
    query.set_period(data::v1::MONTH);
  else if (period == "quarter")
    query.set_period(data::v1::QUARTER);
  else if (period == "year")
    query.set_period(data::v1::YEAR);
  else
    throw std::invalid_argument("invalid daily dataset page query");
  query.set_dataset_id(text(p, "id"));
  validate_id(text(p, "id"));
  query.set_offset(p.at("offset").get<std::uint64_t>());
  query.set_limit(p.at("limit").get<unsigned>());
  query.set_include_macd(p.at("include_macd").get<bool>());
  query.set_begin_day(p.at("start").get<std::string>());
  query.set_end_day(p.at("end").get<std::string>());
  for (const auto& date : {query.begin_day(), query.end_day()})
    if (!date.empty())
      (void)parse_trading_date(date);
  if (!query.begin_day().empty() && !query.end_day().empty() && query.begin_day() > query.end_day())
    throw std::invalid_argument("invalid daily dataset page query");
  return query;
}
void Application::Impl::register_data_commands() {
  command("data.history.usage", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id"});
    data::v1::HistoryUsage usage;
    usage.set_dataset_id(text(p, "id"));
    (void)protocol::decode_history_usage(usage);
    if (!task_client || !data_client)
      throw std::invalid_argument("connect the data and task services first");
    co_return co_await history_usage(p);
  });
  command("data.download.update.plan", [this](const json& p) -> PolledTask<Response> {
    const auto query = protocol::encode_history_update_query(p);
    if (!data_client)
      throw std::invalid_argument("connect the data and task services first");
    const auto client = data_client;
    const auto generation = data_task_generation;
    const auto plan = (co_await PollFuture{client->history_update_plan(query)});
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict, "data/task service selection changed during archive query");
    auto result = snapshot();
    result.values["history_update_plan"] = protocol::decode_history_update_plan(plan);
    co_return result;
  });
  command("data.download.update.submit", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "query", "plan_id", "token"});
    const auto query = protocol::encode_history_update_query(p.at("query"));
    if (!task_client || !data_client)
      throw std::invalid_argument("connect the data and task services first");
    const auto client = task_client;
    const auto reader = data_client;
    const auto generation = data_task_generation;
    const auto plan = (co_await PollFuture{reader->history_update_plan(query)});
    if (plan.id() != text(p, "plan_id"))
      throw std::invalid_argument("history update plan changed; preview again");
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict, "data/task service selection changed during archive query");
    const auto source = plan.has_minutes() ? plan.minutes().source() : plan.daily().source();
    const auto credential = (co_await source_credential(source, text(p, "token", true)));
    data::v1::DownloadAuthorizationRequest authorization;
    authorization.set_task_instance(client->endpoint().session);
    authorization.set_task_id(text(p, "id"));
    authorization.set_credential(credential);
    if (plan.has_minutes())
      *authorization.mutable_minutes() = plan.minutes();
    else
      *authorization.mutable_daily() = plan.daily();
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict, "data/task service selection changed during archive query");
    (co_await run<void>([&]() -> PolledTask<void> {
      const auto reference = (co_await PollFuture{reader->authorize_download(authorization)});
      (co_await PollFuture{client->submit_download(text(p, "id"), reference)});
    }));
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict,
                  "data/task service selection changed during operation; inspect the "
                  "original services before retrying");
    co_return snapshot();
  });
  command("data.dataset.saved", [this](const json& p) -> PolledTask<Response> {
    fields(p, {});
    if (!data_client)
      throw std::invalid_argument("connect the data and task services first");
    const auto client = data_client;
    const auto generation = data_task_generation;
    auto library = (co_await PollFuture{client->saved_datasets()});
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict, "data/task service selection changed during archive query");
    auto result = snapshot();
    result.values["saved_datasets"] = std::move(library);
    co_return result;
  });
  command("data.dataset.save", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"name"});
    if (!data_client)
      throw std::invalid_argument("connect the data and task services first");
    if (!dataset_series.empty())
      throw std::invalid_argument("a named dataset cannot hold a dominant series");
    data::v1::NamedDataset saved;
    saved.set_version(1);
    saved.set_name(text(p, "name"));
    for (const auto& item : selected()) {
      *saved.add_selections() = item.request;
      saved.add_content_revisions(item.dataset.revision());
    }
    saved.set_id(protocol::named_dataset_revision(saved));
    protocol::validate_named_dataset(saved);
    const auto client = data_client;
    const auto generation = data_task_generation;
    (co_await PollFuture{client->save_dataset(saved)});
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict, "data/task service selection changed during archive query");
    co_return snapshot();
  });
  // Restores a saved dataset as the whole selection, after every input still
  // resolves to the recorded content.
  command("data.dataset.use", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id"});
    if (!data_client)
      throw std::invalid_argument("connect the data and task services first");
    const auto client = data_client;
    const auto generation = data_task_generation;
    const auto selection_generation = dataset_selection_generation;
    auto restored = (co_await run<std::vector<DatasetSelection>>(
        [&]() -> PolledTask<std::vector<DatasetSelection>> {
          const auto saved = (co_await PollFuture{client->saved_dataset(text(p, "id"))});
          std::vector<DatasetSelection> result;
          for (int i = 0; i < saved.selections_size(); ++i) {
            const auto& input = saved.selections(i);
            auto data = (co_await PollFuture{client->bar_dataset(input)});
            if (data.revision() != saved.content_revisions(i))
              throw std::invalid_argument("saved dataset revision mismatch");
            result.push_back(resolved_selection(input, std::move(data)));
          }
          co_return result;
        }));
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict, "data/task service selection changed during archive query");
    if (selection_generation != dataset_selection_generation)
      throw Error(ErrorCode::conflict, "dataset selection changed during resolution");
    selections = std::move(restored);
    dataset_series.clear();
    ++dataset_selection_generation;
    co_return snapshot();
  });
  // Archive listing and per-version coverage; reads that never publish.
  for (const auto* method : {"data.datasets", "data.coverage"})
    command(method,
            [this, coverage = std::string_view(method) ==
                              "data.coverage"](const json& p) -> PolledTask<Response> {
              fields(p, {"venue", "product", "contract_id", "source"});
              if (!data_client)
                throw std::invalid_argument("connect the data and task services first");
              const auto reader = data_client;
              const auto generation = data_task_generation;
              data::v1::HistoryFilter filter;
              filter.set_venue(p.value("venue", ""));
              filter.set_product(p.value("product", ""));
              filter.set_contract_id(p.value("contract_id", ""));
              filter.set_source(p.value("source", ""));
              auto rows = (co_await run<json>([&]() -> PolledTask<json> {
                co_return coverage ? (co_await PollFuture{reader->coverage(filter)})
                                   : (co_await PollFuture{reader->datasets(filter)});
              }));
              if (generation != data_task_generation)
                throw Error(ErrorCode::conflict,
                            "data/task service selection changed during archive query");
              auto result = snapshot();
              result.values[coverage ? "history_coverage" : "history_datasets"] = std::move(rows);
              co_return result;
            });
  command("data.daily.page", [this](const json& p) -> PolledTask<Response> {
    const auto query = daily_page_query(p);
    if (!data_client)
      throw std::invalid_argument("connect the data and task services first");
    const auto reader = data_client;
    const auto generation = data_task_generation;
    auto page = (co_await PollFuture{reader->daily_page(query)});
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict, "data service selection changed during daily query");
    auto result = read_published(json::object());
    result.values["daily_page"] = std::move(page);
    co_return result;
  });
  command("data.download.daily.submit", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "source", "contract_id", "requests_per_minute", "token", "catalog_cutoff_ns"});
    if (!task_client)
      throw std::invalid_argument("connect the data and task services first");
    const auto client = task_client;
    const auto reader = data_client;
    const auto generation = data_task_generation;
    const auto& rpm = p.at("requests_per_minute");
    if (!rpm.is_number_integer() || rpm < 1 || rpm > 500)
      throw std::invalid_argument("invalid daily download definition");
    if (text(p, "source") != history_source ||
        text(p, "catalog_cutoff_ns") != std::to_string(history_cutoff))
      throw std::invalid_argument("contract catalog changed; select the contract again");
    const auto found =
        std::ranges::find(*history_contracts, text(p, "contract_id"),
                          [](const HistoryListing& item) { return item.identity.key(); });
    if (found == history_contracts->end())
      throw std::invalid_argument("load and select a dated futures contract first");
    const auto end =
        std::min(found->delist_date, format_shanghai_time(history_cutoff).substr(0, 10));
    data::v1::DailyDownload input;
    input.set_version(2);
    input.set_source(text(p, "source"));
    input.set_contract_id(found->identity.key());
    input.set_source_instrument(found->source_instrument);
    input.set_begin_day(found->list_date);
    input.set_end_day(end);
    input.set_requests_per_minute(rpm.get<unsigned>());
    data::v1::DownloadAuthorizationRequest authorization;
    authorization.set_task_instance(client->endpoint().session);
    authorization.set_task_id(text(p, "id"));
    authorization.set_credential(
        (co_await source_credential(input.source(), text(p, "token", true))));
    *authorization.mutable_daily() = input;
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict, "data/task service selection changed during archive query");
    const auto reference = co_await PollFuture{reader->authorize_download(authorization)};
    co_await PollFuture{client->submit_download(text(p, "id"), reference)};
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict,
                  "data/task service selection changed during operation; inspect the "
                  "original services before retrying");
    co_return snapshot();
  });
  command("data.minutes.page", [this](const json& p) -> PolledTask<Response> {
    const auto query = minute_page_query(p);
    if (!data_client)
      throw std::invalid_argument("connect the data and task services first");
    const auto reader = data_client;
    const auto generation = data_task_generation;
    auto page = (co_await PollFuture{reader->minute_page(query)});
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict, "data service selection changed during minute query");
    auto result = read_published(json::object());
    result.values["history_page"] = std::move(page);
    co_return result;
  });
  // Contract catalog from a data source; apply it only to the still-selected client.
  command("data.contracts.load", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"source", "exchange", "product", "token"});
    const auto credential = (co_await source_credential(text(p, "source"), text(p, "token", true)));
    const auto client = data_client;
    if (!client)
      throw std::invalid_argument("connect the data and task services first");
    auto catalog = (co_await PollFuture{
        client->catalog(text(p, "source"), credential, text(p, "exchange"), text(p, "product"))});
    if (data_client != client)
      throw Error(ErrorCode::conflict, "data connection changed; reload catalog");
    history_source = text(p, "source");
    history_contracts = std::make_shared<const std::vector<HistoryListing>>(std::move(catalog));
    history_exchange = text(p, "exchange");
    history_product = text(p, "product");
    history_cutoff = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    co_return snapshot();
  });
  command("data.download.minutes.submit", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "source", "contract_id", "interval_minutes", "requests_per_minute", "token",
               "catalog_cutoff_ns"});
    if (!task_client)
      throw std::invalid_argument("connect the data and task services first");
    const auto client = task_client;
    const auto reader = data_client;
    const auto generation = data_task_generation;
    for (const auto* name : {"interval_minutes", "requests_per_minute"}) {
      const auto& value = p.at(name);
      if (!value.is_number_integer() || value < 1 ||
          value > (std::string_view(name) == "interval_minutes" ? 1440 : 500))
        throw std::invalid_argument("invalid minute download definition");
    }
    data::v1::MinuteDownload input;
    input.set_version(2);
    input.set_source(text(p, "source"));
    input.set_contract_id(text(p, "contract_id"));
    input.set_interval_minutes(p.at("interval_minutes").get<unsigned>());
    input.set_requests_per_minute(p.at("requests_per_minute").get<unsigned>());
    if (text(p, "source") != history_source ||
        text(p, "catalog_cutoff_ns") != std::to_string(history_cutoff))
      throw std::invalid_argument("contract catalog changed; select the contract again");
    const auto found =
        std::ranges::find(*history_contracts, input.contract_id(),
                          [](const HistoryListing& item) { return item.identity.key(); });
    if (found == history_contracts->end())
      throw std::invalid_argument("load and select a dated futures contract first");
    input.set_source_instrument(found->source_instrument);
    input.set_begin_ns(parse_shanghai_time(found->list_date + " 00:00:00"));
    input.set_end_ns(std::min(history_cutoff / 1000000000 * 1000000000,
                              parse_shanghai_time(found->delist_date + " 23:59:59")));
    data::v1::DownloadAuthorizationRequest authorization;
    authorization.set_task_instance(client->endpoint().session);
    authorization.set_task_id(text(p, "id"));
    authorization.set_credential(
        (co_await source_credential(input.source(), text(p, "token", true))));
    *authorization.mutable_minutes() = input;
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict, "data/task service selection changed during archive query");
    const auto reference = co_await PollFuture{reader->authorize_download(authorization)};
    co_await PollFuture{client->submit_download(text(p, "id"), reference)};
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict,
                  "data/task service selection changed during operation; inspect the "
                  "original services before retrying");
    co_return snapshot();
  });
  // Adds a contract's bars to the portfolio selection, or replaces the same
  // contract. The contract comes from the archive's unified identity; only the
  // units a data source does not provide are entered by the user.
  command("data.dataset.select", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"source_dataset_ids", "settlement_dataset_ids", "begin_day", "end_day",
               "price_increment", "multiplier"});
    if (!data_client)
      throw std::invalid_argument("data service is not connected");
    const auto selection_generation = dataset_selection_generation;
    const auto client = data_client;
    const auto generation = data_task_generation;
    auto next = (co_await run<DatasetSelection>([&]() -> PolledTask<DatasetSelection> {
      const auto request =
          contract_request((co_await PollFuture{client->datasets({})}), p.at("source_dataset_ids"),
                           p.at("settlement_dataset_ids"), p);
      co_return resolved_selection(request, (co_await PollFuture{client->bar_dataset(request)}));
    }));
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict, "data/task service selection changed during archive query");
    if (selection_generation != dataset_selection_generation)
      throw Error(ErrorCode::conflict, "dataset selection changed during resolution");
    if (series_of(next.dataset.contract()))
      throw std::invalid_argument(
          "this product is selected as a dominant series; remove the series first");
    ++dataset_selection_generation;
    const auto same = std::ranges::find_if(selections, [&](const DatasetSelection& item) {
      return item.dataset.contract().venue() == next.dataset.contract().venue() &&
             item.dataset.contract().symbol() == next.dataset.contract().symbol();
    });
    if (same != selections.end())
      *same = std::move(next);
    else if (selections.size() == max_portfolio_contracts)
      throw std::invalid_argument("a portfolio holds at most 20 contracts");
    else
      selections.push_back(std::move(next));
    co_return snapshot();
  });
  // Month contracts of one product as its dominant series: one archive
  // version of bars and one of daily settlements per month. The data service
  // works out the schedule; the months that trade become the selection.
  command("data.dataset.series", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"source_dataset_ids", "settlement_dataset_ids", "begin_day", "end_day",
               "price_increment", "multiplier"});
    if (!data_client)
      throw std::invalid_argument("data service is not connected");
    const auto& sources = p.at("source_dataset_ids");
    const auto& settlements = p.at("settlement_dataset_ids");
    if (!sources.is_array() || !settlements.is_array() || sources.size() < 2 ||
        sources.size() != settlements.size() || sources.size() > max_portfolio_contracts)
      throw std::invalid_argument("a dominant series requires 2 to 20 month contracts");
    const auto selection_generation = dataset_selection_generation;
    const auto client = data_client;
    const auto generation = data_task_generation;
    auto [months, preview] =
        (co_await run<
            std::pair<std::vector<data::v1::BarDatasetRequest>, data::v1::DominantSeriesPreview>>(
            [&]() -> PolledTask<std::pair<std::vector<data::v1::BarDatasetRequest>,
                                          data::v1::DominantSeriesPreview>> {
              const auto archive = (co_await PollFuture{client->datasets({})});
              std::vector<data::v1::BarDatasetRequest> months;
              for (std::size_t i = 0; i < sources.size(); ++i)
                months.push_back(contract_request(archive, json::array({sources[i]}),
                                                  json::array({settlements[i]}), p));
              auto preview = (co_await PollFuture{client->dominant_series(months)});
              co_return std::pair{std::move(months), std::move(preview)};
            }));
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict, "data/task service selection changed during archive query");
    if (selection_generation != dataset_selection_generation)
      throw Error(ErrorCode::conflict, "dataset selection changed during resolution");
    const auto& product = months.front().contract();
    // The series replaces whatever was selected for this product.
    auto kept = selections;
    std::erase_if(kept, [&](const DatasetSelection& item) {
      return item.dataset.contract().venue() == product.venue() &&
             item.dataset.contract().product() == product.product();
    });
    auto remaining = dataset_series;
    std::erase_if(remaining, [&](const DatasetSeries& item) {
      return item.summary.at("venue") == product.venue() &&
             item.summary.at("product") == product.product();
    });
    std::size_t contracts = months.size();
    for (const auto& item : remaining)
      contracts += item.months.size();
    for (const auto& item : kept)
      if (std::ranges::none_of(remaining, [&](const DatasetSeries& series) {
            return series.summary.at("venue") == item.dataset.contract().venue() &&
                   series.summary.at("product") == item.dataset.contract().product();
          }))
        ++contracts;
    if (contracts > max_portfolio_contracts)
      throw std::invalid_argument("a portfolio holds at most 20 contracts");
    json symbols = json::array(), rolls = json::array();
    for (int i = 0; i < preview.months_size(); ++i) {
      const auto& dataset = preview.datasets(i);
      symbols.push_back(dataset.contract().symbol());
      rolls.push_back(
          {{"trading_day", preview.schedule().rolls(i).trading_day()},
           {"symbol", dataset.contract().symbol()},
           {"factor", Decimal::from_raw(preview.schedule().rolls(i).factor().units()).str()}});
      kept.push_back(resolved_selection(months[preview.months(i)], dataset));
    }
    remaining.push_back({std::move(months),
                         {{"venue", product.venue()},
                          {"product", product.product()},
                          {"symbols", std::move(symbols)},
                          {"rolls", std::move(rolls)}}});
    ++dataset_selection_generation;
    selections = std::move(kept);
    dataset_series = std::move(remaining);
    co_return snapshot();
  });
  // Removing a month of a dominant series removes the whole series.
  command("data.dataset.remove", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"venue", "symbol"});
    ++dataset_selection_generation;
    const auto found = std::ranges::find_if(selections, [&](const DatasetSelection& item) {
      return item.dataset.contract().venue() == text(p, "venue") &&
             item.dataset.contract().symbol() == text(p, "symbol");
    });
    if (found == selections.end())
      throw std::invalid_argument("this contract is not selected");
    if (const auto series = series_of(found->dataset.contract())) {
      const auto product = found->dataset.contract();
      std::erase_if(selections, [&](const DatasetSelection& item) {
        return item.dataset.contract().venue() == product.venue() &&
               item.dataset.contract().product() == product.product();
      });
      dataset_series.erase(dataset_series.begin() + static_cast<std::ptrdiff_t>(*series));
    } else
      selections.erase(found);
    co_return snapshot();
  });
  command("data.dataset.clear", [this](const json& p) -> PolledTask<Response> {
    fields(p, {});
    ++dataset_selection_generation;
    selections.clear();
    dataset_series.clear();
    co_return snapshot();
  });
}
} // namespace asterion::terminal
