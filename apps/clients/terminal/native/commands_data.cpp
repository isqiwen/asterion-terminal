#include "application_impl.hpp"
#include <asterion/protocol/resource.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/task_execution.hpp>
#include <asterion/domain/history_identity.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <stdexcept>
#include <set>

namespace asterion::terminal {
namespace {

DatasetSelection
resolved_selection(const data::proto::BarDatasetRequest& request, data::proto::BarDataset dataset)
{
    const auto& bars = dataset.bars();
    const auto& first = bars.Get(0);
    const auto& last = bars.Get(bars.size() - 1);
    // The days it settled on without bars: it did not trade, or bars are missing.
    json uncovered = json::array();
    auto bar = bars.begin();
    for (const auto& day : dataset.days()) {
        while (bar != bars.end() && bar->trading_day() < day.trading_day())
            ++bar;
        if (bar == bars.end() || bar->trading_day() != day.trading_day())
            uncovered.push_back(day.trading_day());
    }
    // The contract with the units the data gave it, and the days its price
    // increment changed.
    json changes = json::array();
    for (const auto& change : dataset.price_increment_changes())
        changes.push_back(
            {{"trading_day", change.trading_day()},
                {"value", Decimal::from_raw(change.value().units()).str()}});
    json summary = protocol::decode_bar_dataset_request(request);
    summary.update(
        {{"contract", protocol::decode_contract(dataset.contract())},
            {"price_increment_changes", std::move(changes)},
            {"venue", dataset.contract().venue()},
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

// What a download of one contract asks the data service for: the contract
// list of the history source's own data source, under the snapshot the user
// chose the contract from.
data::proto::TableContractsQuery contract_list(const json& p, const HistoryIdentity& contract)
{
    data::proto::TableContractsQuery query;
    query.set_snapshot(p.at("snapshot").get<std::string>());
    query.set_source(p.at("source").get<std::string>());
    query.set_exchange(contract.venue);
    query.set_product(contract.product);
    if (query.snapshot().size() != 64
        || query.snapshot().find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid contract list query");
    return query;
}

// The time up to which a contract still trading is downloaded: the contract
// list's reply named it, so a resubmission asks for the same range.
std::int64_t download_cutoff(const json& p)
{
    const auto& text = p.at("cutoff_ns").get_ref<const std::string&>();
    std::int64_t cutoff = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), cutoff);
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                         .count();
    if (error != std::errc{} || end != text.data() + text.size() || cutoff <= 0 || cutoff > now)
        throw std::invalid_argument("invalid download cutoff");
    return cutoff;
}

// The request for one contract. Its daily facts and units come from the
// market-wide tables under the snapshot the user saw; `sources` are archive
// versions of its minute bars, or none for its daily bars in that table.
data::proto::BarDatasetRequest
contract_request(const HistoryIdentity& identity, const json& sources, const json& p)
{
    return protocol::encode_bar_dataset_request(
        {{"source_dataset_ids", sources},
            {"daily", p.at("daily")},
            {"begin_day", p.at("begin_day")},
            {"end_day", p.at("end_day")},
            {"contract", identity.key()}});
}

// The contract an archive version is of: a selection takes its identity from
// the archive, not from the user.
HistoryIdentity archived_contract(const Json& archive, const json& sources)
{
    if (!sources.is_array() || sources.empty())
        throw std::invalid_argument("dataset requires 1..32 source versions");
    for (const auto& item : archive.at("items"))
        if (item.at("id") == sources.at(0))
            return HistoryIdentity::parse(item.at("contract_id").get<std::string>());
    throw std::invalid_argument("historical dataset is unavailable");
}

} // namespace

data::proto::MinutePageQuery minute_page_query(const json& p)
{
    fields(p, {"id", "offset", "limit", "start", "end", "include_macd"});
    if (!p.at("include_macd").is_boolean())
        throw std::invalid_argument("invalid minute dataset page query");
    if (!p.at("offset").is_number_integer() || p.at("offset") < 0 || p.at("offset") > 60000000
        || !p.at("limit").is_number_integer() || p.at("limit") < 1 || p.at("limit") > 200)
        throw std::invalid_argument("invalid minute dataset page query");
    data::proto::MinutePageQuery query;
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

void Application::Impl::adopt_selection(DatasetSelection next)
{
    if (series_of(next.dataset.contract()))
        throw std::invalid_argument(
            "this product is selected as a dominant series; remove the series first");
    ++dataset_selection_generation;
    const auto same = std::ranges::find_if(selections, [&](const DatasetSelection& item) {
        return item.dataset.contract().venue() == next.dataset.contract().venue()
            && item.dataset.contract().symbol() == next.dataset.contract().symbol();
    });
    // What the portfolio trades at a time: each contract outside a series, and
    // each series as one.
    const auto traded = dataset_series.size()
        + static_cast<std::size_t>(
            std::ranges::count_if(selections, [&](const DatasetSelection& item) {
                return !series_of(item.dataset.contract());
            }));
    if (same != selections.end())
        *same = std::move(next);
    else if (traded == max_portfolio_contracts)
        throw std::invalid_argument("a portfolio holds at most 20 contracts and dominant series");
    else if (selections.size() == max_account_contracts)
        throw std::invalid_argument(
            "a selection holds at most 256 month contracts; narrow a dominant series");
    else
        selections.push_back(std::move(next));
}

void Application::Impl::adopt_series(
    DatasetSeries series,
    const data::proto::DominantSeriesPreview& preview,
    const std::vector<data::proto::BarDatasetRequest>& traded)
{
    const auto& product = preview.datasets(0).contract();
    // The series replaces whatever was selected for this product.
    auto kept = selections;
    std::erase_if(kept, [&](const DatasetSelection& item) {
        return item.dataset.contract().venue() == product.venue()
            && item.dataset.contract().product() == product.product();
    });
    auto remaining = dataset_series;
    std::erase_if(remaining, [&](const DatasetSeries& item) {
        return item.summary.at("venue") == product.venue()
            && item.summary.at("product") == product.product();
    });
    // The portfolio trades each contract outside a series, and each series as
    // one. The account keeps every month that trades, and a submission lists
    // every month a series was given (a product lists none).
    std::size_t listed = series.months.size(), outside = 0;
    for (const auto& item : remaining)
        listed += item.months.size();
    for (const auto& item : kept)
        if (std::ranges::none_of(remaining, [&](const DatasetSeries& other) {
                return other.summary.at("venue") == item.dataset.contract().venue()
                    && other.summary.at("product") == item.dataset.contract().product();
            }))
            ++outside;
    if (outside + remaining.size() + 1 > max_portfolio_contracts)
        throw std::invalid_argument("a portfolio holds at most 20 contracts and dominant series");
    if (listed + outside > max_account_contracts
        || kept.size() + traded.size() > max_account_contracts)
        throw std::invalid_argument(
            "a selection holds at most 256 month contracts; narrow a dominant series");
    json symbols = json::array(), rolls = json::array();
    for (int i = 0; i < preview.datasets_size(); ++i) {
        const auto& dataset = preview.datasets(i);
        symbols.push_back(dataset.contract().symbol());
        rolls.push_back(
            {{"trading_day", preview.schedule().rolls(i).trading_day()},
                {"symbol", dataset.contract().symbol()},
                {"factor", Decimal::from_raw(preview.schedule().rolls(i).factor().units()).str()}});
        kept.push_back(resolved_selection(traded[static_cast<std::size_t>(i)], dataset));
    }
    series.summary = {
        {"venue", product.venue()},
        {"product", product.product()},
        {"symbols", std::move(symbols)},
        {"rolls", std::move(rolls)},
        // Trading days on which the term structure is known.
        {"terms", preview.schedule().terms_size()}
    };
    remaining.push_back(std::move(series));
    ++dataset_selection_generation;
    selections = std::move(kept);
    dataset_series = std::move(remaining);
}

void Application::Impl::register_data_commands()
{
    command("resource.inspect", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"object"});
        const auto object = p.at("object");
        fields(object, {"node", "service", "domain", "kind", "id", "version"});
        const auto domain = text(object, "domain"), kind = text(object, "kind"),
                   id = text(object, "id"), version = text(object, "version");
        if (domain != "data" && domain != "research")
            throw std::invalid_argument("unsupported resource domain");
        const bool data = domain == "data";
        if ((data && !data_client) || (!data && !research_client))
            throw Error(ErrorCode::unavailable, "resource owner is not connected");
        const auto endpoint = data ? data_client->endpoint() : research_client->endpoint();
        if (endpoint.owner_node_id != text(object, "node")
            || endpoint.session != text(object, "service"))
            throw Error(ErrorCode::conflict, "resource belongs to another selected owner");
        const auto generation = data_task_generation;
        json value;
        if (data) {
            if (version != id)
                throw Error(ErrorCode::conflict, "Data resource identity changed");
            if (kind == "minute_version")
                value = co_await PollFuture{data_client->minute_retirement(id, false)};
            else if (kind == "table_snapshot")
                value = co_await PollFuture{data_client->table_retirement(id, false)};
            else
                throw std::invalid_argument("unsupported Data resource kind");
        } else {
            if (kind != "backtest" && kind != "factor" && kind != "backtest_result"
                && kind != "factor_result" && kind != "account_review"
                && kind != "account_review_result" && kind != "performance_evaluation"
                && kind != "performance_evaluation_result")
                throw std::invalid_argument("unsupported Research resource kind");
            value = co_await PollFuture{research_client->retirement(id, false)};
            const bool result = kind == "backtest_result" || kind == "factor_result"
                || kind == "account_review_result" || kind == "performance_evaluation_result";
            const auto task_kind =
                (kind == "performance_evaluation" || kind == "performance_evaluation_result")
                ? "performance_evaluation"
                : (kind == "account_review" || kind == "account_review_result") ? "account_review"
                : (kind == "backtest" || kind == "backtest_result")             ? "backtest"
                                                                                : "factor";
            if (value.at("task").at("kind") != task_kind)
                throw Error(ErrorCode::conflict, "Research resource kind changed");
            if ((result ? value.at("task").at("result_digest") : value.at("version")) != version)
                throw Error(ErrorCode::conflict, "Research resource version changed");
        }
        if (data)
            value["selected"] = std::ranges::any_of(selections, [&](const auto& selection) {
                if (kind == "table_snapshot")
                    return selection.request.daily().snapshot() == id;
                const auto& versions = selection.request.source_dataset_ids();
                return std::ranges::find(versions, id) != versions.end();
            });
        if (generation != data_task_generation)
            throw Error(ErrorCode::conflict, "resource owner changed during inspection");
        auto result = snapshot();
        result.values["resource_inspection"] = {
            {"object", object},
            {"maintenance", std::move(value)}
        };
        co_return result;
    });
    for (const auto* method :
        {"data.observation.admit", "data.observation.execute", "data.observation.read"})
        command(
            method,
            [this, method = std::string(method)](const json& p) -> PolledTask<Response> {
                if (method == "data.observation.admit")
                    fields(p, {"id", "source"});
                else
                    fields(p, {"id"});
                if (!data_client)
                    throw Error(ErrorCode::unavailable, "select the resource owner service first");
                data::proto::ObservationQuery query;
                if (method == "data.observation.admit") {
                    auto* admission = query.mutable_admit();
                    admission->set_id(text(p, "id"));
                    const auto& source = p.at("source");
                    fields(source, {"node", "service", "domain", "kind", "id", "version"});
                    auto* object = admission->mutable_source();
                    object->mutable_owner()->set_node_id(text(source, "node"));
                    object->mutable_owner()->set_service_id(text(source, "service"));
                    object->mutable_owner()->set_domain(text(source, "domain"));
                    object->set_kind(text(source, "kind"));
                    object->set_id(text(source, "id"));
                    object->set_version(text(source, "version"));
                } else if (method == "data.observation.execute")
                    query.set_execute(text(p, "id"));
                else
                    query.set_read(text(p, "id"));
                const auto client = data_client;
                const auto generation = data_task_generation;
                auto value = co_await PollFuture{client->observation(query)};
                if (client != data_client || generation != data_task_generation)
                    throw Error(
                        ErrorCode::conflict,
                        "resource owner selection changed; reload its catalogue");
                auto result = snapshot();
                result.values["observation_archive"] = std::move(value);
                co_return result;
            });
    for (const auto* method :
        {"data.material.sources",
            "data.material.submit",
            "data.material.task",
            "data.material.read"})
        command(
            method,
            [this, method = std::string(method)](const json& p) -> PolledTask<Response> {
                if (!data_client)
                    throw Error(ErrorCode::unavailable, "select a Data service first");
                const auto client = data_client;
                const auto generation = data_task_generation;
                Json value;
                std::string key;
                if (method == "data.material.sources") {
                    fields(p, {});
                    value = co_await PollFuture{client->material_sources()};
                    key = "material_sources";
                } else if (method == "data.material.submit") {
                    fields(
                        p,
                        {"id",
                            "source_id",
                            "resource_id",
                            "use_scope",
                            "provider_artifact",
                            "max_requests",
                            "max_bytes"});
                    data::proto::MaterialRequest request;
                    request.set_id(text(p, "id"));
                    request.set_source_id(text(p, "source_id"));
                    request.set_resource_id(text(p, "resource_id"));
                    request.set_use_scope(text(p, "use_scope"));
                    request.set_provider_artifact(text(p, "provider_artifact"));
                    for (const auto* name : {"max_requests", "max_bytes"}) {
                        const auto& value = p.at(name);
                        if (!value.is_number_integer() || value < 1
                            || value
                                > (std::string_view(name) == "max_requests" ? 32 : 4 * 1024 * 1024))
                            throw std::invalid_argument("material request budget exceeds bounds");
                    }
                    request.set_max_requests(p.at("max_requests").get<unsigned>());
                    request.set_max_bytes(p.at("max_bytes").get<unsigned>());
                    value = co_await PollFuture{client->submit_material(request)};
                    key = "material_task";
                } else {
                    fields(p, {"id"});
                    const auto id = text(p, "id");
                    if (method == "data.material.read") {
                        value = co_await PollFuture{client->material_read(id)};
                        key = "material_read";
                    } else {
                        value = co_await PollFuture{client->metadata(id)};
                        key = "material_task";
                        if (value.at("kind") != "material_acquisition")
                            throw Error(ErrorCode::conflict, "task is not a material acquisition");
                    }
                }
                if (client != data_client || generation != data_task_generation)
                    throw Error(
                        ErrorCode::conflict,
                        "Data owner changed while acquiring or reading material");
                auto result = snapshot();
                result.values[key] = std::move(value);
                co_return result;
            });
    command("resource.catalogue", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"domain", "kind", "after"});
        const auto domain = text(p, "domain");
        if (domain != "data" && domain != "research")
            throw std::invalid_argument("unsupported resource domain");
        const bool data = domain == "data";
        if ((data && !data_client) || (!data && !research_client))
            throw Error(ErrorCode::unavailable, "select the resource owner service first");
        resource::proto::CatalogueQuery query;
        query.set_kind(text(p, "kind"));
        query.set_after(text(p, "after", true));
        query.set_limit(64);
        const auto generation = data_task_generation;
        auto value = co_await PollFuture{
            data ? data_client->resources(query) : research_client->resources(query)
        };
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "resource owner selection changed; reload its catalogue");
        auto result = snapshot();
        result.values["resources"] = std::move(value);
        co_return result;
    });
    for (const auto method : {"data.tables.retirement.preview", "data.tables.retirement.execute"})
        command(
            method,
            [this, method = std::string(method)](const json& p) -> PolledTask<Response> {
                if (method == "data.tables.retirement.execute")
                    fields(p, {"id", "preview_digest"});
                else
                    fields(p, {"id"});
                if (!data_client)
                    throw std::invalid_argument("connect the data service first");
                const auto id = text(p, "id");
                const bool execute = method == "data.tables.retirement.execute";
                const bool selected = std::ranges::any_of(selections, [&](const auto& selection) {
                    return selection.request.daily().snapshot() == id;
                });
                if (execute && selected)
                    throw Error(
                        ErrorCode::conflict,
                        "table snapshot is selected by this window; clear the draft first");
                const auto client = data_client;
                auto value = co_await PollFuture{client->table_retirement(
                    id,
                    execute,
                    execute ? text(p, "preview_digest") : std::string{})};
                if (client != data_client)
                    throw Error(
                        ErrorCode::conflict,
                        "Data service changed; inspect the original snapshot owner");
                value["selected"] = selected;
                auto result = snapshot();
                result.values["table_retirement"] = std::move(value);
                co_return result;
            });
    command("data.tables.retirements", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"after"});
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        const auto client = data_client;
        auto value = co_await PollFuture{client->table_retirements(text(p, "after", true))};
        if (client != data_client)
            throw Error(ErrorCode::conflict, "Data service changed during snapshot inventory");
        auto result = snapshot();
        result.values["table_retirements"] = std::move(value);
        co_return result;
    });
    command("data.history.retirement.preview", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id"});
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        const auto reader = data_client;
        auto value = co_await PollFuture{reader->minute_retirement(text(p, "id"), false)};
        if (reader != data_client)
            throw Error(ErrorCode::conflict, "data service changed during retirement preview");
        value["selected"] = std::ranges::any_of(selections, [&](const auto& selection) {
            const auto& ids = selection.request.source_dataset_ids();
            return std::ranges::find(ids, text(p, "id")) != ids.end();
        });
        auto result = snapshot();
        result.values["minute_retirement"] = std::move(value);
        co_return result;
    });
    command("data.history.retirement.execute", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id"});
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        const auto id = text(p, "id");
        for (const auto& selection : selections)
            if (std::ranges::find(selection.request.source_dataset_ids(), id)
                != selection.request.source_dataset_ids().end())
                throw Error(
                    ErrorCode::conflict,
                    "minute version is selected by this window; clear the draft first");
        const auto reader = data_client;
        auto value = co_await PollFuture{reader->minute_retirement(id, true)};
        if (reader != data_client)
            throw Error(
                ErrorCode::conflict,
                "data service changed; inspect the original service before retrying retirement");
        auto result = snapshot();
        result.values["minute_retirement"] = std::move(value);
        co_return result;
    });
    command("data.history.retirements", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"after"});
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        const auto reader = data_client;
        auto value = co_await PollFuture{reader->minute_retirements(text(p, "after", true))};
        if (reader != data_client)
            throw Error(ErrorCode::conflict, "data service changed during retirement listing");
        auto result = snapshot();
        result.values["minute_retirements"] = std::move(value);
        co_return result;
    });
    command("data.history.usage", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id"});
        data::proto::HistoryUsage usage;
        usage.set_dataset_id(text(p, "id"));
        (void)protocol::decode_history_usage(usage);
        if (!data_client)
            throw std::invalid_argument("data service is not connected");
        co_return co_await history_usage(p);
    });
    command("data.download.update.plan", [this](const json& p) -> PolledTask<Response> {
        const auto query = protocol::encode_history_update_query(p);
        if (!data_client)
            throw std::invalid_argument("data service is not connected");
        const auto client = data_client;
        const auto generation = data_client;
        const auto plan = (co_await PollFuture{client->history_update_plan(query)});
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        auto result = snapshot();
        result.values["history_update_plan"] = protocol::decode_history_update_plan(plan);
        co_return result;
    });
    command("data.download.update.submit", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "query", "plan_id", "token"});
        const auto query = protocol::encode_history_update_query(p.at("query"));
        if (!data_client)
            throw std::invalid_argument("data service is not connected");
        const auto reader = data_client;
        const auto client = reader;
        const auto generation = data_client;
        const auto plan = (co_await PollFuture{reader->history_update_plan(query)});
        if (plan.id() != text(p, "plan_id"))
            throw std::invalid_argument("history update plan changed; preview again");
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        if (!plan.conflicts().empty())
            throw Error(
                ErrorCode::conflict,
                "repair preview is blocked by reviewed closure conflicts; inspect the preview");
        const auto credential =
            (co_await source_credential(plan.minutes().source(), text(p, "token", true)));
        data::proto::DownloadAuthorizationRequest authorization;
        authorization.set_task_instance(client->endpoint().session);
        authorization.set_task_id(text(p, "id"));
        authorization.set_credential(credential);
        *authorization.mutable_minutes() = plan.minutes();
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        (co_await run<void>([&]() -> PolledTask<void> {
            const auto reference = (co_await PollFuture{reader->authorize_download(authorization)});
            (co_await PollFuture{client->submit_download(text(p, "id"), reference)});
        }));
        if (generation != data_client)
            throw Error(
                ErrorCode::conflict,
                "data service selection changed during operation; inspect the " "original service before retrying");
        co_return snapshot();
    });
    // Market-wide tables. An update only names what to read; the data service
    // plans the partitions when the task runs and publishes each as it arrives.
    command("data.tables.refresh.plan", [this](const json& p) -> PolledTask<Response> {
        const auto query = protocol::encode_table_refresh_query(p);
        if (!data_client)
            throw std::invalid_argument("data service is not connected");
        const auto reader = data_client;
        const auto generation = data_client;
        auto plan = co_await PollFuture{reader->table_refresh_plan(query)};
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        auto result = read_published(json::object());
        result.values["table_refresh_plan"] = protocol::decode_table_refresh_plan(plan);
        co_return result;
    });
    command("data.tables.refresh.submit", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "query", "plan_id", "token", "connection_id"});
        const auto query = protocol::encode_table_refresh_query(p.at("query"));
        if (!data_client)
            throw std::invalid_argument("data service is not connected");
        const auto reader = data_client;
        if (reader->owner_status().at("connection_id") != text(p, "connection_id"))
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        const auto generation = data_client;
        const auto plan = co_await PollFuture{reader->table_refresh_plan(query)};
        if (plan.id() != text(p, "plan_id"))
            throw Error(ErrorCode::conflict, "table refresh plan changed; preview again");
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        data::proto::DownloadAuthorizationRequest authorization;
        authorization.set_task_instance(reader->endpoint().session);
        authorization.set_task_id(text(p, "id"));
        *authorization.mutable_table_update() = plan.update();
        authorization.set_credential(
            co_await provider_credential(query.provider(), text(p, "token", true)));
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        const auto reference = co_await PollFuture{reader->authorize_download(authorization)};
        data::proto::AcquisitionTaskRequest submit;
        submit.mutable_submit()->set_id(text(p, "id"));
        submit.mutable_submit()->set_authorization(reference);
        co_await PollFuture{reader->acquisition_task(std::move(submit))};
        co_return snapshot();
    });
    command("data.tables.update.submit", [this](const json& p) -> PolledTask<Response> {
        fields(
            p,
            {"id", "provider", "tables", "begin_day", "end_day", "requests_per_minute", "token"});
        if (!data_client)
            throw std::invalid_argument("data service is not connected");
        const auto reader = data_client;
        const auto client = reader;
        const auto generation = data_client;
        data::proto::DownloadAuthorizationRequest authorization;
        *authorization.mutable_table_update() = protocol::encode_table_update(
            {{"provider", p.at("provider")},
                {"tables", p.at("tables")},
                {"begin_day", p.at("begin_day")},
                {"end_day", p.at("end_day")},
                {"requests_per_minute", p.at("requests_per_minute")}});
        authorization.set_task_instance(client->endpoint().session);
        authorization.set_task_id(text(p, "id"));
        authorization.set_credential(
            (co_await provider_credential(text(p, "provider"), text(p, "token", true))));
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        const auto reference = co_await PollFuture{reader->authorize_download(authorization)};
        co_await PollFuture{client->submit_download(text(p, "id"), reference)};
        if (generation != data_client)
            throw Error(
                ErrorCode::conflict,
                "data service selection changed during operation; inspect the " "original service before retrying");
        co_return snapshot();
    });
    command("data.tables.coverage", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        const auto reader = data_client;
        const auto generation = data_client;
        auto coverage = (co_await PollFuture{reader->table_coverage()});
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        auto result = read_published(json::object());
        result.values["table_coverage"] = std::move(coverage);
        co_return result;
    });
    command("data.tables.page", [this](const json& p) -> PolledTask<Response> {
        const auto query = protocol::encode_table_page_query(p);
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        const auto reader = data_client;
        const auto generation = data_client;
        auto page = (co_await PollFuture{reader->table_page(query)});
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        auto result = read_published(json::object());
        result.values["table_page"] = std::move(page);
        co_return result;
    });
    command("data.tables.daily", [this](const json& p) -> PolledTask<Response> {
        const auto query = protocol::encode_table_daily_query(p);
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        const auto reader = data_client;
        const auto generation = data_client;
        auto page = (co_await PollFuture{reader->table_daily(query)});
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        auto result = read_published(json::object());
        result.values["table_daily"] = std::move(page);
        co_return result;
    });
    command("data.tables.listed_contracts", [this](const json& p) -> PolledTask<Response> {
        const auto query = protocol::encode_table_listed_contracts_query(p);
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        const auto reader = data_client;
        const auto generation = data_client;
        auto catalog = (co_await PollFuture{reader->table_listed_contracts(query)});
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        auto result = read_published(json::object());
        result.values["table_listed_contracts"] = std::move(catalog);
        co_return result;
    });
    command("data.tables.products", [this](const json& p) -> PolledTask<Response> {
        const auto query = protocol::encode_table_products_query(p);
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        const auto reader = data_client;
        const auto generation = data_client;
        auto products = (co_await PollFuture{reader->table_products(query)});
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        auto result = read_published(json::object());
        result.values["table_products"] = std::move(products);
        co_return result;
    });
    command("data.dataset.saved", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        const auto client = data_client;
        const auto generation = data_client;
        auto library = (co_await PollFuture{client->saved_datasets()});
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        auto result = snapshot();
        result.values["saved_datasets"] = std::move(library.at("items"));
        co_return result;
    });
    command("data.dataset.save", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"name"});
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        if (!dataset_series.empty())
            throw std::invalid_argument("a named dataset cannot hold a dominant series");
        data::proto::NamedDataset saved;
        saved.set_name(text(p, "name"));
        for (const auto& item : selected()) {
            *saved.add_selections() = item.request;
            saved.add_content_revisions(item.dataset.revision());
        }
        saved.set_id(protocol::named_dataset_revision(saved));
        protocol::validate_named_dataset(saved);
        const auto client = data_client;
        const auto generation = data_client;
        (co_await PollFuture{client->save_dataset(saved)});
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        co_return snapshot();
    });
    // Restores a saved dataset as the whole selection, after every input still
    // resolves to the recorded content.
    command("data.dataset.use", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id"});
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        const auto client = data_client;
        const auto generation = data_client;
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
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        if (selection_generation != dataset_selection_generation)
            throw Error(ErrorCode::conflict, "dataset selection changed during resolution");
        selections = std::move(restored);
        dataset_series.clear();
        ++dataset_selection_generation;
        co_return snapshot();
    });
    // Archive listing, and its minute versions against the daily table the
    // coverage check names; reads that never publish.
    for (const auto* method : {"data.datasets", "data.coverage"})
        command(
            method,
            [this, coverage = std::string_view(method) == "data.coverage"](
                const json& p) -> PolledTask<Response> {
                if (coverage)
                    fields(p, {"venue", "product", "contract_id", "source", "daily"});
                else
                    fields(p, {"venue", "product", "contract_id", "source"});
                if (!data_client)
                    throw std::invalid_argument("connect the data service first");
                const auto reader = data_client;
                const auto generation = data_client;
                data::proto::HistoryCoverageQuery query;
                auto& filter = *query.mutable_filter();
                filter.set_venue(p.value("venue", ""));
                filter.set_product(p.value("product", ""));
                filter.set_contract_id(p.value("contract_id", ""));
                filter.set_source(p.value("source", ""));
                if (coverage)
                    *query.mutable_daily() = protocol::encode_daily_table(p.at("daily"));
                auto rows = (co_await run<json>([&]() -> PolledTask<json> {
                    co_return coverage ? (co_await PollFuture{reader->coverage(query)})
                                       : (co_await PollFuture{reader->datasets(query.filter())});
                }));
                if (generation != data_client)
                    throw Error(
                        ErrorCode::conflict,
                        "data service selection changed during archive query");
                auto result = snapshot();
                if (coverage) {
                    result.values["history_coverage"] = std::move(rows);
                } else {
                    result.values["history_datasets"] = std::move(rows.at("items"));
                }
                co_return result;
            });
    command("data.minutes.range", [this](const json& p) -> PolledTask<Response> {
        const auto query = protocol::encode_minute_range_query(p);
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        const auto reader = data_client;
        const auto generation = data_client;
        auto evidence = (co_await PollFuture{reader->minute_range(query)});
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during minute query");
        auto result = read_published(json::object());
        result.values["minute_range"] = std::move(evidence);
        co_return result;
    });
    command("data.minutes.evidence", [this](const json& p) -> PolledTask<Response> {
        const auto query = protocol::encode_minute_evidence_query(p);
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        const auto reader = data_client;
        const auto generation = data_client;
        auto evidence = (co_await PollFuture{reader->minute_evidence(query)});
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during minute query");
        auto result = read_published(json::object());
        result.values["minute_evidence"] = std::move(evidence);
        co_return result;
    });
    command("data.minutes.page", [this](const json& p) -> PolledTask<Response> {
        const auto query = minute_page_query(p);
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        const auto reader = data_client;
        const auto generation = data_client;
        auto page = (co_await PollFuture{reader->minute_page(query)});
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during minute query");
        auto result = read_published(json::object());
        result.values["history_page"] = std::move(page);
        co_return result;
    });
    // The contracts a history source downloads, from its own data source's
    // contract list under a snapshot. The reply names the time up to which a
    // contract still trading would be fetched; a download submits it back.
    command("data.tables.contracts", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"snapshot", "source", "exchange", "product"});
        const auto product =
            protocol::table_instrument(text(p, "exchange") + "/" + text(p, "product"));
        if (!product.delivery_month.empty())
            throw std::invalid_argument("invalid contract list query");
        const auto query =
            contract_list(p, HistoryIdentity{product.venue, product.product, "2000-01"});
        if (!data_client)
            throw std::invalid_argument("connect the data service first");
        const auto reader = data_client;
        const auto generation = data_client;
        const auto cutoff = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                .count();
        const auto listed = (co_await PollFuture{reader->table_contracts(query)});
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        json items = json::array();
        for (const auto& item : listed)
            items.push_back(
                {{"code", item.identity.key()},
                    {"name", item.name},
                    {"list_date", item.list_date},
                    {"delist_date", item.delist_date},
                    {"multiplier", item.multiplier ? json(item.multiplier->str()) : json(nullptr)},
                    {"per_unit", item.per_unit ? json(item.per_unit->str()) : json(nullptr)},
                    {"trade_unit", item.trade_unit ? json(*item.trade_unit) : json(nullptr)},
                    {"quote_unit", item.quote_unit ? json(*item.quote_unit) : json(nullptr)}});
        auto result = read_published(json::object());
        result.values["table_contracts"] = {
            {"source", query.source()},
            {"snapshot", query.snapshot()},
            {"exchange", query.exchange()},
            {"product", query.product()},
            {"cutoff_ns", std::to_string(cutoff)},
            {"items", std::move(items)}
        };
        co_return result;
    });
    command("data.download.minutes.submit", [this](const json& p) -> PolledTask<Response> {
        fields(
            p,
            {"id",
                "source",
                "contract_id",
                "interval_minutes",
                "requests_per_minute",
                "token",
                "snapshot",
                "cutoff_ns"});
        if (!data_client)
            throw std::invalid_argument("data service is not connected");
        const auto reader = data_client;
        const auto client = reader;
        const auto generation = data_client;
        for (const auto* name : {"interval_minutes", "requests_per_minute"}) {
            const auto& value = p.at(name);
            if (!value.is_number_integer() || value < 1
                || value > (std::string_view(name) == "interval_minutes" ? 1440 : 500))
                throw std::invalid_argument("invalid minute download definition");
        }
        data::proto::MinuteDownload input;
        input.set_source(text(p, "source"));
        input.set_contract_id(text(p, "contract_id"));
        input.set_interval_minutes(p.at("interval_minutes").get<unsigned>());
        input.set_requests_per_minute(p.at("requests_per_minute").get<unsigned>());
        const auto contract = HistoryIdentity::parse(input.contract_id());
        const auto cutoff = download_cutoff(p);
        const auto listed =
            (co_await PollFuture{reader->table_contracts(contract_list(p, contract))});
        const auto found =
            std::ranges::find(listed, contract.key(), [](const HistoryListing& item) {
                return item.identity.key();
            });
        if (found == listed.end())
            throw std::invalid_argument("load and select a dated futures contract first");
        input.set_source_instrument(found->source_instrument);
        input.set_begin_ns(parse_shanghai_time(found->list_date + " 00:00:00"));
        input.set_end_ns(
            std::min(
                cutoff / 1000000000 * 1000000000,
                parse_shanghai_time(found->delist_date + " 23:59:59")));
        data::proto::DownloadAuthorizationRequest authorization;
        authorization.set_task_instance(client->endpoint().session);
        authorization.set_task_id(text(p, "id"));
        authorization.set_credential(
            (co_await source_credential(input.source(), text(p, "token", true))));
        *authorization.mutable_minutes() = input;
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        const auto reference = co_await PollFuture{reader->authorize_download(authorization)};
        co_await PollFuture{client->submit_download(text(p, "id"), reference)};
        if (generation != data_client)
            throw Error(
                ErrorCode::conflict,
                "data service selection changed during operation; inspect the " "original service before retrying");
        co_return snapshot();
    });
    // Adds a contract's bars to the portfolio selection, or replaces the same
    // contract. The contract comes from the archive's unified identity, its
    // units from the market-wide tables.
    command("data.dataset.select", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"source_dataset_ids", "daily", "begin_day", "end_day"});
        if (!data_client)
            throw std::invalid_argument("data service is not connected");
        const auto selection_generation = dataset_selection_generation;
        const auto client = data_client;
        const auto generation = data_client;
        auto next = (co_await run<DatasetSelection>([&]() -> PolledTask<DatasetSelection> {
            const auto request = contract_request(
                archived_contract(
                    (co_await PollFuture{client->datasets({})}),
                    p.at("source_dataset_ids")),
                p.at("source_dataset_ids"),
                p);
            co_return resolved_selection(
                request,
                (co_await PollFuture{client->bar_dataset(request)}));
        }));
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        if (selection_generation != dataset_selection_generation)
            throw Error(ErrorCode::conflict, "dataset selection changed during resolution");
        adopt_selection(std::move(next));
        co_return snapshot();
    });
    // A contract's daily bars in the market-wide daily table: the days it traded
    // on from the first day to the last. No archive version is chosen.
    command("data.dataset.daily", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"contract_id", "daily", "begin_day", "end_day"});
        if (!data_client)
            throw std::invalid_argument("data service is not connected");
        const auto request =
            contract_request(HistoryIdentity::parse(text(p, "contract_id")), json::array(), p);
        const auto selection_generation = dataset_selection_generation;
        const auto client = data_client;
        const auto generation = data_client;
        auto next =
            resolved_selection(request, (co_await PollFuture{client->bar_dataset(request)}));
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        if (selection_generation != dataset_selection_generation)
            throw Error(ErrorCode::conflict, "dataset selection changed during resolution");
        adopt_selection(std::move(next));
        co_return snapshot();
    });
    // Month contracts of one product as its dominant series: one archive
    // version of bars per month. The data service works out the schedule from
    // what the daily table says each month settled at and held; the months that
    // trade become the selection.
    command("data.dataset.series", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"source_dataset_ids", "daily", "begin_day", "end_day"});
        if (!data_client)
            throw std::invalid_argument("data service is not connected");
        const auto& sources = p.at("source_dataset_ids");
        if (!sources.is_array() || sources.size() < 2 || sources.size() > max_portfolio_contracts)
            throw std::invalid_argument("a dominant series requires 2 to 20 month contracts");
        const auto selection_generation = dataset_selection_generation;
        const auto client = data_client;
        const auto generation = data_client;
        auto awaited = (co_await run<std::pair<
                std::vector<data::proto::BarDatasetRequest>,
                data::proto::DominantSeriesPreview>>(
            [&]() -> PolledTask<std::pair<
                      std::vector<data::proto::BarDatasetRequest>,
                      data::proto::DominantSeriesPreview>> {
                const auto archive = (co_await PollFuture{client->datasets({})});
                std::vector<data::proto::BarDatasetRequest> months;
                for (std::size_t i = 0; i < sources.size(); ++i)
                    months.push_back(contract_request(
                        archived_contract(archive, json::array({sources[i]})),
                        json::array({sources[i]}),
                        p));
                auto preview = (co_await PollFuture{client->dominant_series(months)});
                co_return std::pair{std::move(months), std::move(preview)};
            }));
        auto& [months, preview] = awaited;
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        if (selection_generation != dataset_selection_generation)
            throw Error(ErrorCode::conflict, "dataset selection changed during resolution");
        // Each month that trades is selected with the request it was resolved from.
        std::vector<data::proto::BarDatasetRequest> traded;
        for (const auto index : preview.months())
            traded.push_back(months[index]);
        adopt_series({std::move(months), std::nullopt, {}}, preview, traded);
        co_return snapshot();
    });
    // A product's daily bars in the market-wide daily table as its dominant
    // series: the data service finds the months that traded in the range.
    command("data.dataset.product", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"product", "daily", "begin_day", "end_day"});
        if (!data_client)
            throw std::invalid_argument("data service is not connected");
        const auto instrument = protocol::table_instrument(text(p, "product"));
        if (!instrument.delivery_month.empty())
            throw std::invalid_argument(
                "a dominant series is read of a product, not of a contract");
        const auto request = protocol::encode_product_bars_request(
            {{"venue", instrument.venue},
                {"product", instrument.product},
                {"begin_day", p.at("begin_day")},
                {"end_day", p.at("end_day")},
                {"daily", p.at("daily")}});
        const auto selection_generation = dataset_selection_generation;
        const auto client = data_client;
        const auto generation = data_client;
        const auto preview = (co_await PollFuture{client->dominant_series(request)});
        if (generation != data_client)
            throw Error(ErrorCode::conflict, "data service selection changed during archive query");
        if (selection_generation != dataset_selection_generation)
            throw Error(ErrorCode::conflict, "dataset selection changed during resolution");
        // Each month that trades is selected as its own daily bars over the days
        // the series reads of it.
        std::vector<data::proto::BarDatasetRequest> traded;
        for (const auto& dataset : preview.datasets())
            traded.push_back(protocol::bar_dataset_selection(dataset));
        adopt_series({{}, request, {}}, preview, traded);
        co_return snapshot();
    });
    // Removing a month of a dominant series removes the whole series.
    command("data.dataset.remove", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"venue", "symbol"});
        ++dataset_selection_generation;
        const auto found = std::ranges::find_if(selections, [&](const DatasetSelection& item) {
            return item.dataset.contract().venue() == text(p, "venue")
                && item.dataset.contract().symbol() == text(p, "symbol");
        });
        if (found == selections.end())
            throw std::invalid_argument("this contract is not selected");
        if (const auto series = series_of(found->dataset.contract())) {
            const auto product = found->dataset.contract();
            std::erase_if(selections, [&](const DatasetSelection& item) {
                return item.dataset.contract().venue() == product.venue()
                    && item.dataset.contract().product() == product.product();
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
