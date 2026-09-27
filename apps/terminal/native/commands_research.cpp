#include "application_impl.hpp"

namespace asterion::terminal {
// Research tasks: backtest, factor, calendar and data publication.
void Application::Impl::register_research_commands() {
  core.command("research.local", "node.manage", [this](const json& p) {
    fields(p, {});
    if (!nodes.contains("local"))
      nodes.emplace("local", std::make_unique<NodeClient>(local_node()));
    auto next = std::make_unique<ResearchClient>(nodes.at("local")->local_research());
    research = std::move(next);
    research_result = nullptr;
    return snapshot();
  });
  core.command("research.attach", "node.manage", [this](const json& p) {
    fields(p, {"id", "service"});
    auto next = std::make_unique<ResearchClient>(
        nodes.at(text(p, "id"))->service_endpoint(text(p, "service"), "research"));
    research = std::move(next);
    research_result = nullptr;
    return snapshot();
  });
  core.command("research.submit", "node.manage", [this](const json& p) {
    fields(p, {"id", "days", "calendar_task", "fast", "slow", "quantity", "deposit",
               "margin_per_lot", "open_fee", "close_today_fee", "close_yesterday_fee",
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
    json costs = json::object();
    for (auto key : {"margin_per_lot", "open_fee", "close_today_fee", "close_yesterday_fee"})
      costs[key] = text(p, key);
    const json paper_input{{"version", 1},
                           {"type", "historical_paper"},
                           {"contract", contract},
                           {"costs", costs},
                           {"risk", risk_parameters(p)},
                           {"deposit", text(p, "deposit")},
                           {"ticks", preview->replay}};
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
    const json input{
        {"version", 5},
        {"calendar_publication", calendar_publication},
        {"days", days},
        {"dataset_revision", protocol::dataset_revision(encoded)},
        {"paper", paper_input},
        {"sma",
         {{"fast", p.at("fast")}, {"slow", p.at("slow")}, {"quantity", text(p, "quantity")}}}};
    research->submit(text(p, "id"), protocol::encode_backtest(input));
    return snapshot();
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
    research_result = research->result(text(p, "id"));
    return snapshot();
  });
}
} // namespace asterion::terminal
