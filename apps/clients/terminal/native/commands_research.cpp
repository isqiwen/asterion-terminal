#include "application_impl.hpp"
#include "tushare.hpp"
#include <stdexcept>
#include <algorithm>

namespace asterion::terminal {
data::v1::MinutePageQuery minute_page_query(const json& p) {
  fields(p, {"id", "offset", "limit", "start", "end", "include_macd"});
  if (!p.at("include_macd").is_boolean())
    throw std::invalid_argument("invalid minute dataset page query");
  if (!p.at("offset").is_number_integer() || p.at("offset") < 0 || p.at("offset") > 60000000 ||
      !p.at("limit").is_number_integer() || p.at("limit") < 1 || p.at("limit") > 200)
    throw std::invalid_argument("invalid minute dataset page query");
  data::v1::MinutePageQuery query;
  query.set_task_id(text(p, "id"));
  query.set_include_macd(p.at("include_macd").get<bool>());
  query.set_offset(p.at("offset").get<std::uint64_t>());
  query.set_limit(p.at("limit").get<unsigned>());
  if (!p.at("start").is_string() || !p.at("end").is_string())
    throw std::invalid_argument("invalid minute dataset page query");
  if (!p.at("start").get_ref<const std::string&>().empty())
    query.set_begin_ns(tushare::parse_time(text(p, "start")));
  if (!p.at("end").get_ref<const std::string&>().empty())
    query.set_end_ns(tushare::parse_time(text(p, "end")));
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
  query.set_task_id(text(p, "id"));
  validate_id(query.task_id());
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
// Research tasks: backtest, factor, calendar and data publication.
void Application::Impl::register_research_commands() {
  core.command("research.daily.page", "node.manage", [this](const json& p) {
    (void)daily_page_query(p);
    if (!research)
      throw std::invalid_argument("connect research service first");
    auto published = read_published(json::object());
    return published.is_null() ? snapshot() : published;
  });
  core.command("research.daily.submit", "node.manage", [this](const json& p) {
    fields(p, {"id", "ts_code", "requests_per_minute", "token", "catalog_cutoff_ns"});
    if (!research)
      throw std::invalid_argument("connect research service first");
    const auto& rpm = p.at("requests_per_minute");
    if (!rpm.is_number_integer() || rpm < 1 || rpm > 500)
      throw std::invalid_argument("invalid daily download definition");
    if (text(p, "catalog_cutoff_ns") != std::to_string(history_cutoff))
      throw std::invalid_argument("contract catalog changed; select the contract again");
    const auto found =
        std::ranges::find(history_contracts, text(p, "ts_code"), &tushare::FuturesListing::ts_code);
    if (found == history_contracts.end())
      throw std::invalid_argument("load and select a dated futures contract first");
    const auto range = tushare::daily_contract_range(*found, history_cutoff);
    data::v1::DailyDownload input;
    input.set_version(1);
    input.set_ts_code(found->ts_code);
    input.set_begin_day(format_trading_date(range.begin));
    input.set_end_day(format_trading_date(range.end));
    input.set_requests_per_minute(rpm.get<unsigned>());
    research->submit(text(p, "id"), input, text(p, "token"));
    return snapshot();
  });
  core.command("research.minutes.page", "node.manage", [this](const json& p) {
    (void)minute_page_query(p);
    if (!research)
      throw std::invalid_argument("connect research service first");
    auto published = read_published(json::object());
    return published.is_null() ? snapshot() : published;
  });
  core.command("research.contracts.load", "node.manage", [this](const json& p) {
    fields(p, {"exchange", "product", "token"});
    return snapshot();
  });
  core.command("research.minutes.submit", "node.manage", [this](const json& p) {
    fields(p, {"id", "ts_code", "interval_minutes", "requests_per_minute", "token",
               "catalog_cutoff_ns"});
    if (!research)
      throw std::invalid_argument("connect research service first");
    for (const auto* name : {"interval_minutes", "requests_per_minute"}) {
      const auto& value = p.at(name);
      if (!value.is_number_integer() || value < 1 || value > 500)
        throw std::invalid_argument("invalid minute download definition");
    }
    data::v1::MinuteDownload input;
    input.set_version(1);
    input.set_ts_code(text(p, "ts_code"));
    input.set_interval_minutes(p.at("interval_minutes").get<unsigned>());
    input.set_requests_per_minute(p.at("requests_per_minute").get<unsigned>());
    if (text(p, "catalog_cutoff_ns") != std::to_string(history_cutoff))
      throw std::invalid_argument("contract catalog changed; select the contract again");
    const auto found =
        std::ranges::find(history_contracts, input.ts_code(), &tushare::FuturesListing::ts_code);
    if (found == history_contracts.end())
      throw std::invalid_argument("load and select a dated futures contract first");
    const auto range = tushare::contract_range(*found, input.interval_minutes(), history_cutoff);
    input.set_begin_ns(range.begin_ns);
    input.set_end_ns(range.end_ns);
    research->submit(text(p, "id"), input, text(p, "token"));
    return snapshot();
  });
  core.command("research.local", "node.manage", [this](const json& p) {
    fields(p, {});
    if (!nodes.contains("local"))
      nodes.emplace("local", std::make_unique<NodeClient>(local_node()));
    auto next = std::make_shared<ResearchClient>(nodes.at("local")->local_research());
    research = std::move(next);
    research_result = nullptr;
    ++research_generation;
    return snapshot();
  });
  core.command("research.attach", "node.manage", [this](const json& p) {
    fields(p, {"id", "service"});
    auto next = std::make_shared<ResearchClient>(
        nodes.at(text(p, "id"))->service_endpoint(text(p, "service"), "research"));
    research = std::move(next);
    research_result = nullptr;
    ++research_generation;
    return snapshot();
  });
  core.command("research.submit", "node.manage", [this](const json& p) {
    fields_with_costs(p, {"id", "days", "calendar_task", "fast", "slow", "quantity", "deposit",
                          "max_order_quantity", "max_gross_quantity", "max_working_orders"});
    if (!research)
      throw std::invalid_argument("research service is not connected");
    const auto preview = core.resources().resolve<PreviewState>("terminal", "preview").lock();
    if (preview->dataset.is_null())
      throw std::invalid_argument("import historical futures data first");
    json contract = json::object();
    for (auto key : {"venue", "symbol", "currency", "price_increment", "quantity_increment",
                     "multiplier", "product", "delivery_month"})
      contract[key] = preview->dataset.at(key);
    const auto costs = cost_parameters(p);
    // Validated before the braced initializer (GCC < 13 PR66139 leak).
    const auto risk = risk_parameters(p);
    const auto deposit = text(p, "deposit");
    const json paper_input{
        {"version", 1}, {"type", "historical_paper"}, {"contract", contract},    {"costs", costs},
        {"risk", risk}, {"deposit", deposit},         {"ticks", preview->replay}};
    const auto encoded = protocol::encode_input(paper_input);
    json calendar_publication = nullptr;
    json days = p.at("days");
    const auto calendar_task = p.at("calendar_task").get<std::string>();
    if (!calendar_task.empty()) {
      if (!days.is_null())
        throw std::invalid_argument("published calendar cannot be combined with edited days");
      const auto evidence = research->result(calendar_task);
      if (evidence.at("kind") != "calendar_import")
        throw std::invalid_argument("selected task is not a calendar publication");
      calendar_publication = evidence.at("result");
      days = calendar_publication.at("calendar").at("days");
    }
    const auto revision = protocol::dataset_revision(encoded);
    const auto quantity = text(p, "quantity");
    const json input{
        {"version", 5},
        {"calendar_publication", calendar_publication},
        {"days", days},
        {"dataset_revision", revision},
        {"paper", paper_input},
        {"sma", {{"fast", p.at("fast")}, {"slow", p.at("slow")}, {"quantity", quantity}}}};
    research->submit(text(p, "id"), protocol::encode_backtest(input));
    return snapshot();
  });
  core.command("research.daily-factor.submit", "node.manage", [this](const json& p) {
    fields(p, {"id", "source_task_id", "lookback", "horizon", "evaluation"});
    validate_id(text(p, "id"));
    auto definition = p;
    definition.erase("id");
    (void)protocol::encode_daily_factor_request(definition);
    if (!research)
      throw std::invalid_argument("research service is not connected");
    auto published = read_published(json::object());
    return published.is_null() ? snapshot() : published;
  });
  core.command("research.factor.submit", "node.manage", [this](const json& p) {
    fields(p, {"id", "lookbacks", "horizon", "evaluation"});
    if (!research)
      throw std::invalid_argument("research service is not connected");
    const auto preview = core.resources().resolve<PreviewState>("terminal", "preview").lock();
    if (preview->dataset.is_null())
      throw std::invalid_argument("import historical futures data first");
    json contract = json::object();
    for (auto key : {"venue", "symbol", "currency", "price_increment", "quantity_increment",
                     "multiplier", "product", "delivery_month"})
      contract[key] = preview->dataset.at(key);
    auto input = protocol::encode_factor({{"version", 4},
                                          {"dataset_revision", ""},
                                          {"contract", contract},
                                          {"ticks", preview->replay},
                                          {"lookbacks", p.at("lookbacks")},
                                          {"horizon", p.at("horizon")},
                                          {"evaluation", p.at("evaluation")}});
    input.set_dataset_revision(protocol::factor_dataset_revision(input));
    research->submit(text(p, "id"), input);
    return snapshot();
  });
  core.command("research.calendar.submit", "node.manage", [this](const json& p) {
    fields(p, {"id", "path"});
    if (!research)
      throw std::invalid_argument("research service is not connected");
    const auto preview = core.resources().resolve<PreviewState>("terminal", "preview").lock();
    if (preview->dataset.is_null())
      throw std::invalid_argument("select the futures contract first");
    const auto name = text(p, "path");
    if (name.find('\0') != std::string::npos)
      throw std::invalid_argument("invalid calendar source path");
    const std::filesystem::path path(std::u8string(name.begin(), name.end()));
    constexpr std::size_t limit = 1024 * 1024;
    if (!path.is_absolute() || std::filesystem::is_symlink(path) ||
        !std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > limit)
      throw std::invalid_argument(
          "calendar source requires an absolute regular file of at most 1 MiB");
    std::ifstream file(path, std::ios::binary);
    if (!file)
      throw std::runtime_error("cannot read calendar CSV");
    std::string bytes(limit + 1, '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(file.gcount()));
    if (file.bad() || !file.eof() || bytes.size() > limit)
      throw std::invalid_argument("calendar source size or read failure");
    json contract = json::object();
    for (auto key : {"venue", "symbol", "currency", "price_increment", "quantity_increment",
                     "multiplier", "product", "delivery_month"})
      contract[key] = preview->dataset.at(key);
    data::v1::CalendarCsvSnapshot input;
    input.set_version(1);
    *input.mutable_contract() = protocol::encode_contract(contract);
    const auto filename = path.filename().u8string();
    input.set_source_name(std::string(filename.begin(), filename.end()));
    input.set_source_sha256(sha256_bytes(bytes));
    input.set_contents(std::move(bytes));
    static_cast<void>(protocol::decode_calendar_snapshot(input));
    research->submit(text(p, "id"), input);
    return snapshot();
  });
  core.command("research.data.submit", "node.manage", [this](const json& p) {
    fields(p, {"id"});
    if (!research)
      throw std::invalid_argument("research service is not connected");
    const auto preview = core.resources().resolve<PreviewState>("terminal", "preview").lock();
    if (!preview->source)
      throw std::invalid_argument("select a nonempty CSV with at most 10000 "
                                  "events and 4 MiB to publish");
    research->submit(text(p, "id"), *preview->source);
    return snapshot();
  });
  core.command("research.data.use", "node.manage", [this](const json& p) {
    fields(p, {"id"});
    if (!research)
      throw std::invalid_argument("research service is not connected");
    const auto response = research->result(text(p, "id"));
    if (response.at("kind") != "data_import")
      throw std::invalid_argument("not a data publication task");
    const auto& publication = response.at("result");
    const auto& data = publication.at("dataset");
    const auto& rows = data.at("ticks");
    auto next = data.at("contract");
    Decimal quantity;
    for (const auto& row : rows)
      quantity = quantity + Decimal::parse(row.at("quantity").get<std::string>());
    json recent = json::array();
    for (std::size_t i = rows.size() > 240 ? rows.size() - 240 : 0; i < rows.size(); ++i)
      recent.push_back(rows.at(i));
    next.update({{"filename", publication.at("source_name")},
                 {"count", rows.size()},
                 {"quantity", quantity.str()},
                 {"first_timestamp_ns", rows.front().at("timestamp_ns")},
                 {"last_timestamp_ns", rows.back().at("timestamp_ns")},
                 {"last_price", rows.back().at("price")},
                 {"ticks", recent},
                 {"source", "published_csv"},
                 {"persistent", true},
                 {"specification_source", "user_supplied"},
                 {"publication_ready", false},
                 {"publication_id", publication.at("id")},
                 {"revision", data.at("revision")}});
    auto preview = core.resources().resolve<PreviewState>("terminal", "preview").lock();
    preview->dataset = std::move(next);
    preview->replay = rows;
    preview->source.reset();
    return snapshot();
  });
  core.command("research.action", "node.manage", [this](const json& p) {
    fields(p, {"id", "action"});
    if (!research)
      throw std::invalid_argument("research service is not connected");
    research->action(text(p, "id"), text(p, "action"));
    return snapshot();
  });
  core.command("research.result", "node.manage", [this](const json& p) {
    fields(p, {"id"});
    if (!research)
      throw std::invalid_argument("research service is not connected");
    validate_id(text(p, "id"));
    auto published = read_published(json::object());
    return published.is_null() ? snapshot() : published;
  });
}
} // namespace asterion::terminal
