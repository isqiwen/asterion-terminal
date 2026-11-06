#include "manager.hpp"
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/market.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/durable_file.hpp>
#include <fstream>
#include <regex>
#include <set>

namespace asterion::market_data::acquisition {
namespace fs = std::filesystem;

namespace {

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void require_id(const std::string& id)
{
    static const std::regex pattern("[A-Za-z0-9_-]{1,64}");
    if (!std::regex_match(id, pattern))
        throw std::invalid_argument("invalid acquisition task id");
}

std::string table(const wire::StartAcquisition& request)
{
    return protocol::table_name(request.table());
}

bool pending(const wire::AcquisitionTask& task)
{
    return task.state() == wire::TASK_STATE_RUNNING || task.state() == wire::TASK_STATE_QUEUED;
}

void validate(wire::StartAcquisition& request)
{
    require_id(request.id());
    warehouse::require_provider(request.provider());
    const auto first = day_of(request.first_day()), last = day_of(request.last_day());
    if (first > last || request.first_day() < "1990-01-01")
        throw std::invalid_argument("acquisition needs a range of days from 1990 on");
    if (request.table() == wire::TABLE_OBSERVATIONS)
        throw std::invalid_argument("acquisition table is not implemented");
    if (table(request) == "minutes" && request.contracts().empty()) {
        // Every contract the source holds on those days.
        if (request.exchanges_size())
            throw std::invalid_argument("a minute acquisition of every contract names no exchange");
    } else if (table(request) == "minutes") {
        std::set<std::string> contracts, exchanges;
        for (const auto& contract : request.contracts()) {
            require_contract(contract);
            if (!contracts.insert(contract).second)
                throw std::invalid_argument("duplicate acquisition contract");
            exchanges.insert(contract.substr(0, contract.find('/')));
        }
        if (request.exchanges().empty())
            for (const auto& exchange : exchanges)
                request.add_exchanges(exchange);
        if (std::set<std::string>(request.exchanges().begin(), request.exchanges().end())
            != exchanges)
            throw std::invalid_argument("minute acquisition names its contracts' exchanges only");
    } else if (!request.contracts().empty())
        throw std::invalid_argument("a contract filter is only supported for minutes");
    std::set<std::string> seen;
    for (const auto& exchange : request.exchanges()) {
        require_exchange(exchange);
        if (!seen.insert(exchange).second)
            throw std::invalid_argument("duplicate acquisition exchange");
    }
}

} // namespace

wire::Sources Manager::sources() const
{
    wire::Sources result;
    for (const auto& [provider, factory] : sources_) {
        const auto source = factory({});
        auto* entry = result.add_sources();
        entry->set_provider(provider);
        std::set<std::string> exchanges;
        for (const auto* table :
            {"daily", "minutes", "settlement", "price_ticks", "calendar", "contracts"}) {
            bool supported = false;
            for (const auto* exchange : {"SHFE", "INE", "DCE", "CZCE", "CFFEX", "GFEX"}) {
                if (source->supports(table, exchange)) {
                    exchanges.insert(exchange);
                    supported = true;
                }
            }
            if (supported)
                entry->add_tables(protocol::table_of(table));
        }
        for (const auto& exchange : exchanges)
            entry->add_exchanges(exchange);
    }
    return result;
}

Manager::Manager(
    fs::path directory,
    std::map<std::string, Factory> sources,
    std::function<void()> committed)
    : directory_(std::move(directory) / "acquisition"),
      sources_(std::move(sources)),
      committed_(std::move(committed))
{
    create_directories_durably(directory_);
    for (const auto& [provider, factory] : sources_) {
        if (!factory)
            throw std::invalid_argument("missing source factory");
        stores_[provider] = std::make_unique<warehouse::Store>(
            warehouse::source_directory(directory_.parent_path(), provider),
            provider);
    }
    // An accepted task never silently resumes network requests after a restart.
    for (const auto& entry : fs::directory_iterator(directory_)) {
        if (entry.path().extension() != ".pb")
            continue;
        // A damaged record costs that task alone, never the service's start.
        try {
            auto task = load(entry.path());
            if (pending(task)) {
                task.set_state(wire::TASK_STATE_INTERRUPTED);
                task.mutable_error()->set_code(::common::ERROR_CODE_UNAVAILABLE);
                task.mutable_error()->set_message("Market stopped before acquisition completed");
                task.set_finished_ms(now_ms());
                persist(task);
            }
        } catch (const std::exception& error) {
            log::error("acquisition task ignored: {}", error.what());
        }
    }
}

Manager::~Manager()
{
    stop();
}

void Manager::persist(const wire::AcquisitionTask& task) const
{
    replace_file_durably(directory_ / (task.request().id() + ".pb"), task.SerializeAsString());
}

wire::AcquisitionTask Manager::load(const fs::path& path) const
{
    if (!fs::is_regular_file(path))
        throw Error(ErrorCode::not_found, "acquisition task is missing");
    if (fs::file_size(path) > 65536)
        throw std::runtime_error("acquisition task file is too large");
    std::ifstream input(path, std::ios::binary);
    wire::AcquisitionTask task;
    if (!task.ParseFromIstream(&input) || task.request().id() != path.stem().string()
        || !wire::TaskState_IsValid(task.state()) || task.state() == wire::TASK_STATE_UNSPECIFIED)
        throw std::runtime_error("invalid acquisition task: " + path.string());
    require_id(task.request().id());
    return task;
}

void Manager::reap()
{
    for (auto it = active_.begin(); it != active_.end();) {
        if (pending(it->second->task)
            && it->second->finished.wait_for(std::chrono::seconds(0))
                != std::future_status::ready) {
            ++it;
            continue;
        }
        // Persistence errors are service faults, never silently turned into success.
        it->second->finished.get();
        it = active_.erase(it);
    }
}

wire::AcquisitionTask Manager::submit(wire::StartAcquisition request)
{
    validate(request);
    std::lock_guard lock(mutex_);
    if (stopped_)
        throw Error(ErrorCode::unavailable, "acquisition is stopped");
    reap();
    if (!sources_.contains(request.provider()))
        throw std::invalid_argument("acquisition source is not configured");
    if (fs::exists(directory_ / (request.id() + ".pb")))
        throw Error(ErrorCode::conflict, "acquisition task id already exists");
    std::size_t requested = 0;
    for (const auto& [id, active] : active_) {
        if (active->task.request().provider() == request.provider())
            throw Error(ErrorCode::conflict, "another acquisition for this source is running");
        requested += active->task.request().provider() != "recorder";
    }
    // The recorder's slot is its own: a day's publication never waits for a provider.
    if (request.provider() != "recorder" && requested >= 3)
        throw Error(ErrorCode::resource_exhausted, "acquisition capacity is full");
    auto active = std::make_unique<Active>();
    *active->task.mutable_request() = request;
    active->task.set_state(wire::TASK_STATE_QUEUED);
    active->task.set_started_ms(now_ms());
    persist(active->task);
    const auto accepted = active->task;
    auto* current = active.get();
    active_.emplace(request.id(), std::move(active));
    current->finished =
        workers_
            .submit([this, current, request](std::stop_token shutdown) {
                std::stop_callback cancel(shutdown, [&] {
                    current->cancel.request_stop();
                });
                auto task = current->task;
                std::unique_ptr<Source> source;
                try {
                    check_stop(current->cancel.get_token());
                    task.set_state(wire::TASK_STATE_RUNNING);
                    publish(task, *current);
                    source = sources_.at(request.provider())(current->cancel.get_token());
                    auto query = request;
                    if (query.exchanges().empty())
                        for (const auto* exchange : {"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"})
                            if (source->supports(table(query), exchange))
                                query.add_exchanges(exchange);
                    *task.mutable_request() = query;
                    publish(task, *current);
                    run(*current, *source, *stores_.at(query.provider()), query);
                    {
                        std::lock_guard lock(mutex_);
                        task = current->task;
                    }
                    check_stop(current->cancel.get_token());
                    task.set_state(wire::TASK_STATE_SUCCEEDED);
                } catch (const SourceError& error) {
                    {
                        std::lock_guard lock(mutex_);
                        task = current->task;
                    }
                    task.set_state(
                        error.code == "cancelled" ? wire::TASK_STATE_CANCELLED
                                                  : wire::TASK_STATE_FAILED);
                    task.mutable_error()->set_code(
                        protocol::error_code(
                            error.code == "unsupported"
                                ? ErrorCode::invalid_request
                                : parse_error_code(error.code)
                                      .value_or(ErrorCode::operation_failed)));
                    task.mutable_error()->set_message(error.what());
                } catch (const std::exception& error) {
                    {
                        std::lock_guard lock(mutex_);
                        task = current->task;
                    }
                    task.set_state(wire::TASK_STATE_FAILED);
                    *task.mutable_error() = protocol::error(error);
                    log::error(
                        "acquisition {} failed: provider={} table={} days={}..{} contracts={}: {}",
                        request.id(),
                        request.provider(),
                        table(request),
                        request.first_day(),
                        request.last_day(),
                        request.contracts_size(),
                        error.what());
                }
                if (source)
                    task.set_requests_sent(source->requests());
                task.set_finished_ms(now_ms());
                // The source may have committed data immediately before a status write
                // failed. The warehouse remains authoritative; never roll it back.
                publish(task, *current);
            })
            .share();
    return accepted;
}

void Manager::publish(const wire::AcquisitionTask& task, Active& active)
{
    std::lock_guard lock(mutex_);
    persist(task);
    active.task = task;
}

wire::AcquisitionTask Manager::get(const std::string& id) const
{
    require_id(id);
    std::lock_guard lock(mutex_);
    if (const auto at = active_.find(id); at != active_.end()) {
        if (at->second->finished.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            at->second->finished.get();
        return at->second->task;
    }
    return load(directory_ / (id + ".pb"));
}

wire::AcquisitionTasks Manager::list(const wire::ListAcquisitionTasks& query) const
{
    if (!query.after_id().empty())
        require_id(query.after_id());
    if (!query.provider().empty())
        warehouse::require_provider(query.provider());
    std::lock_guard lock(mutex_);
    for (const auto& [id, active] : active_)
        if (active->finished.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            active->finished.get();
    std::vector<wire::AcquisitionTask> tasks;
    for (const auto& entry : fs::directory_iterator(directory_)) {
        if (entry.path().extension() != ".pb")
            continue;
        try {
            auto task = load(entry.path());
            if (query.provider().empty() || task.request().provider() == query.provider())
                tasks.push_back(std::move(task));
        } catch (const std::exception& error) {
            log::error("acquisition task ignored: {}", error.what());
        }
    }
    std::sort(tasks.begin(), tasks.end(), [](const auto& a, const auto& b) {
        return a.started_ms() != b.started_ms() ? a.started_ms() > b.started_ms()
                                                : a.request().id() < b.request().id();
    });
    auto from = tasks.begin();
    if (!query.after_id().empty()) {
        from = std::find_if(tasks.begin(), tasks.end(), [&](const auto& task) {
            return task.request().id() == query.after_id();
        });
        if (from == tasks.end())
            throw Error(ErrorCode::not_found, "acquisition task not found");
        ++from;
    }
    wire::AcquisitionTasks result;
    for (; from != tasks.end() && result.tasks_size() < 20; ++from)
        *result.add_tasks() = *from;
    if (from != tasks.end())
        result.set_next_id(result.tasks(result.tasks_size() - 1).request().id());
    return result;
}

void Manager::cancel(const std::string& id)
{
    require_id(id);
    std::lock_guard lock(mutex_);
    if (const auto at = active_.find(id); at != active_.end())
        at->second->cancel.request_stop();
    else
        load(directory_ / (id + ".pb"));
}

void Manager::stop()
{
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
    }
    workers_.shutdown();
}

void Manager::run(
    Active& active,
    Source& source,
    warehouse::Store& store,
    const wire::StartAcquisition& request)
{
    wire::AcquisitionTask task;
    {
        std::lock_guard lock(mutex_);
        task = active.task;
    }
    if (request.exchanges().empty())
        throw SourceError("unsupported", "source does not provide the requested table");
    for (const auto& exchange : request.exchanges())
        if (!source.supports(table(request), exchange))
            throw SourceError("unsupported", "source does not provide this table for " + exchange);
    publish(task, active);
    const auto stop = active.cancel.get_token();
    // Provider trading dates use Beijing's civil day, independent of host TZ.
    const auto today = std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now() + std::chrono::hours(8));
    // A provider's day so far would be stored as the whole day and never taken
    // again: other sources stop at yesterday.
    const auto first = day_of(request.first_day()),
               last = std::min(
                   day_of(request.last_day()),
                   source.marks_partial_days() ? today : today - std::chrono::days{1});
    if (last < first)
        return;
    auto writer = store.writer();
    auto version = store.current();
    task.set_published_version(version.identity);
    const auto commit = [&] {
        version = writer.commit();
        if (committed_)
            committed_();
        task.set_published_version(version.identity);
        task.set_requests_sent(source.requests());
        publish(task, active);
    };
    if (table(request) == "minutes" || table(request) == "settlement"
        || table(request) == "price_ticks" || !source.supports("calendar", request.exchanges(0))) {
        // Exchange, day and, for minutes, contract of each partition.
        std::vector<std::tuple<std::string, std::string, std::string>> plan;
        if (table(request) == "minutes" && request.contracts().empty())
            for (auto day = first; day <= last; day += std::chrono::days(1))
                for (const auto& contract : source.minute_contracts(day_text(day)))
                    plan.emplace_back(
                        contract.substr(0, contract.find('/')),
                        day_text(day),
                        contract);
        else if (table(request) == "minutes")
            for (const auto& contract : request.contracts())
                for (auto day = first; day <= last; day += std::chrono::days(1))
                    plan.emplace_back(
                        contract.substr(0, contract.find('/')),
                        day_text(day),
                        contract);
        else
            for (auto day = first; day <= last; day += std::chrono::days(1))
                for (const auto& exchange : request.exchanges()) {
                    if (std::chrono::weekday(day).iso_encoding() < 6)
                        plan.emplace_back(exchange, day_text(day), "");
                }
        task.set_items(plan.size());
        publish(task, active);
        // Every version lists all partitions: publish in batches, and on
        // cancellation or failure keep what was fetched.
        std::size_t unpublished = 0;
        auto published_at = std::chrono::steady_clock::now();
        const auto publish_batch = [&] {
            if (!unpublished)
                return;
            commit();
            unpublished = 0;
            published_at = std::chrono::steady_clock::now();
        };
        try {
            for (const auto& [exchange, day, contract] : plan) {
                check_stop(stop);
                auto key = table(request) + "/" + exchange + "/" + day;
                if (!contract.empty())
                    key += "/" + contract.substr(exchange.size() + 1);
                if (request.refetch() || !version.partitions.contains(key)) {
                    MinuteBatch batch;
                    if (table(request) == "minutes")
                        batch = source.minutes(contract, day);
                    else if (table(request) == "settlement")
                        batch.rows = source.settlement(exchange, day);
                    else if (table(request) == "price_ticks")
                        batch.rows = source.price_ticks(exchange, day);
                    else
                        batch.rows = source.daily(exchange, day);
                    check_stop(stop);
                    if (batch.rows.empty())
                        task.set_items_empty(task.items_empty() + 1);
                    else {
                        writer.put(key, batch.rows, {day});
                        if (!batch.observations.empty())
                            writer.put(
                                "observations/" + key.substr(key.find('/') + 1),
                                batch.observations,
                                {day});
                        ++unpublished;
                    }
                }
                if (unpublished >= 200
                    || std::chrono::steady_clock::now() - published_at >= std::chrono::minutes(5))
                    publish_batch();
                task.set_items_done(task.items_done() + 1);
                task.set_requests_sent(source.requests());
                publish(task, active);
            }
        } catch (...) {
            publish_batch();
            throw;
        }
        publish_batch();
        return;
    }
    for (const auto& exchange : request.exchanges()) {
        check_stop(stop);
        if (table(request) == "daily" || table(request) == "calendar") {
            for (int year = int(std::chrono::year_month_day(first).year());
                year <= int(std::chrono::year_month_day(last).year());
                ++year) {
                const auto key = "calendar/" + exchange + "/" + std::to_string(year);
                if (request.refetch() || !version.partitions.contains(key)
                    || year == int(std::chrono::year_month_day(today).year()))
                    writer.put(key, source.calendar(exchange, year));
            }
        }
        if (table(request) == "daily" || table(request) == "contracts")
            writer.put("contracts/" + exchange, source.contracts(exchange));
    }
    check_stop(stop);
    commit();
    if (table(request) != "daily")
        return;
    std::vector<std::pair<std::string, std::string>> plan;
    for (const auto& exchange : request.exchanges()) {
        std::set<std::string> stored;
        if (!request.refetch())
            for (const auto& [key, part] : version.partitions)
                if (key.starts_with("daily/" + exchange + "/"))
                    stored.insert(part.days.begin(), part.days.end());
        for (int year = int(std::chrono::year_month_day(first).year());
            year <= int(std::chrono::year_month_day(last).year());
            ++year) {
            for (const auto& row :
                store.read(version, "calendar/" + exchange + "/" + std::to_string(year))) {
                const auto day = std::get<std::string>(row[1]);
                if (std::get<bool>(row[2]) && day >= request.first_day() && day <= day_text(last)
                    && !stored.contains(day))
                    plan.emplace_back(exchange, day);
            }
        }
    }
    std::sort(plan.begin(), plan.end());
    task.set_items(plan.size());
    publish(task, active);
    Rows rows;
    std::vector<std::string> days;
    std::string exchange, month;
    const auto flush = [&] {
        if (days.empty())
            return;
        writer.merge_days(exchange, month, std::move(rows), std::move(days));
        rows.clear();
        days.clear();
        commit();
    };
    try {
        for (const auto& [venue, day] : plan) {
            if (exchange != venue || month != day.substr(0, 7))
                flush();
            exchange = venue;
            month = day.substr(0, 7);
            check_stop(stop);
            auto batch = source.daily(venue, day);
            if (batch.empty())
                task.set_items_empty(task.items_empty() + 1);
            else {
                rows.insert(
                    rows.end(),
                    std::make_move_iterator(batch.begin()),
                    std::make_move_iterator(batch.end()));
                days.push_back(day);
            }
            task.set_items_done(task.items_done() + 1);
            task.set_requests_sent(source.requests());
            publish(task, active);
        }
    } catch (...) {
        flush();
        throw;
    }
    flush();
}

} // namespace asterion::market_data::acquisition
