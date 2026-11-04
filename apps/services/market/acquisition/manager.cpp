#include "manager.hpp"
#include "warehouse/research.hpp"
#include <asterion/system/log.hpp>
#include <asterion/system/durable_file.hpp>
#include <fstream>
#include <regex>
#include <set>

namespace asterion::market_data::acquisition {
namespace fs = std::filesystem;

namespace {

std::int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void require_id(const std::string& id)
{
    static const std::regex pattern("[A-Za-z0-9_-]{1,64}");
    if (!std::regex_match(id, pattern))
        throw std::invalid_argument("invalid acquisition task id");
}

bool pending(const wire::Task& task)
{
    return task.state() == wire::TASK_STATE_RUNNING || task.state() == wire::TASK_STATE_QUEUED;
}

void validate(wire::Request& request)
{
    require_id(request.id());
    warehouse::require_provider(request.provider());
    const auto first = day_of(request.begin_day()), last = day_of(request.end_day());
    if (first > last || request.begin_day() < "1990-01-01")
        throw std::invalid_argument("acquisition needs a range of days from 1990 on");
    if (request.table() != "daily" && request.table() != "calendar"
        && request.table() != "contracts" && request.table() != "minutes"
        && request.table() != "settlement")
        throw std::invalid_argument("acquisition table is not implemented");
    if (request.table() == "minutes" && request.contract().empty()) {
        // Every contract the source holds on those days.
        if (request.exchanges_size())
            throw std::invalid_argument("a minute acquisition of every contract names no exchange");
    } else if (request.table() == "minutes") {
        require_contract(request.contract());
        const auto exchange = request.contract().substr(0, request.contract().find('/'));
        if (request.exchanges().empty())
            request.add_exchanges(exchange);
        if (request.exchanges_size() != 1 || request.exchanges(0) != exchange)
            throw std::invalid_argument("minute acquisition needs its contract's exchange only");
    } else if (!request.contract().empty())
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
        for (const auto* table : {"daily", "minutes", "settlement", "calendar", "contracts"}) {
            bool supported = false;
            for (const auto* exchange : {"SHFE", "INE", "DCE", "CZCE", "CFFEX", "GFEX"}) {
                if (source->supports(table, exchange)) {
                    exchanges.insert(exchange);
                    supported = true;
                }
            }
            if (supported)
                entry->add_tables(table);
        }
        for (const auto& exchange : exchanges)
            entry->add_exchanges(exchange);
    }
    return result;
}

Manager::Manager(fs::path directory, std::map<std::string, Factory> sources)
    : directory_(std::move(directory) / "acquisition"),
      sources_(std::move(sources)),
      research_(warehouse::source_directory(directory_.parent_path(), "research"), "research")
{
    create_directories_durably(directory_);
    for (const auto& [provider, factory] : sources_) {
        if (!factory)
            throw std::invalid_argument("missing source factory");
        stores_[provider] = std::make_unique<warehouse::Store>(
            warehouse::source_directory(directory_.parent_path(), provider),
            provider);
    }
    // Research reads what the sources hold now, whatever stopped composing
    // before. Recording must start even if it cannot; the next commit retries.
    try {
        warehouse::compose_research(directory_.parent_path(), research_);
    } catch (const std::exception& error) {
        log::error("market research dataset not composed: {}", error.what());
    }
    // An accepted task never silently resumes network requests after a restart.
    for (const auto& entry : fs::directory_iterator(directory_)) {
        if (entry.path().extension() != ".pb")
            continue;
        auto task = load(entry.path());
        if (pending(task)) {
            task.set_state(wire::TASK_STATE_INTERRUPTED);
            task.set_error_code("interrupted");
            task.set_error("Market stopped before acquisition completed");
            task.set_finished_ns(now_ns());
            persist(task);
        }
    }
}

Manager::~Manager()
{
    stop();
}

void Manager::persist(const wire::Task& task) const
{
    replace_file_durably(directory_ / (task.request().id() + ".pb"), task.SerializeAsString());
}

wire::Task Manager::load(const fs::path& path) const
{
    if (!fs::is_regular_file(path))
        throw Error(ErrorCode::not_found, "acquisition task is missing");
    if (fs::file_size(path) > 65536)
        throw std::runtime_error("acquisition task file is too large");
    std::ifstream input(path, std::ios::binary);
    wire::Task task;
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

wire::Task Manager::submit(wire::Request request)
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
    for (const auto& [id, active] : active_)
        if (active->task.request().provider() == request.provider())
            throw Error(ErrorCode::conflict, "another acquisition for this source is running");
    if (active_.size() >= 2)
        throw Error(ErrorCode::resource_exhausted, "acquisition capacity is full");
    auto active = std::make_unique<Active>();
    *active->task.mutable_request() = request;
    active->task.set_state(wire::TASK_STATE_QUEUED);
    active->task.set_started_ns(now_ns());
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
                    run(*current, *source, *stores_.at(request.provider()));
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
                    task.set_error_code(error.code);
                    task.set_error(error.what());
                } catch (const std::exception& error) {
                    {
                        std::lock_guard lock(mutex_);
                        task = current->task;
                    }
                    task.set_state(wire::TASK_STATE_FAILED);
                    task.set_error_code("operation_failed");
                    task.set_error(error.what());
                    log::error(
                        "acquisition {} failed: provider={} table={} days={}..{} contract={}: {}",
                        request.id(),
                        request.provider(),
                        request.table(),
                        request.begin_day(),
                        request.end_day(),
                        request.contract(),
                        error.what());
                }
                if (source)
                    task.set_requests(source->requests());
                task.set_finished_ns(now_ns());
                // The source may have committed data immediately before a status write
                // failed. The warehouse remains authoritative; never roll it back.
                publish(task, *current);
            })
            .share();
    return accepted;
}

void Manager::publish(const wire::Task& task, Active& active)
{
    std::lock_guard lock(mutex_);
    persist(task);
    active.task = task;
}

wire::Task Manager::get(const std::string& id) const
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

wire::Tasks Manager::list(const wire::TasksQuery& query) const
{
    if (!query.after_id().empty())
        require_id(query.after_id());
    if (query.limit() > 128)
        throw std::invalid_argument("too many acquisition tasks requested");
    const std::size_t limit = query.limit() ? query.limit() : 64;
    std::lock_guard lock(mutex_);
    std::set<std::string> ids;
    for (const auto& entry : fs::directory_iterator(directory_)) {
        if (entry.path().extension() != ".pb")
            continue;
        const auto id = entry.path().stem().string();
        if (id > query.after_id())
            ids.insert(id);
        if (ids.size() > limit + 1)
            ids.erase(std::prev(ids.end()));
    }
    wire::Tasks result;
    for (const auto& id : ids) {
        if (result.tasks_size() == static_cast<int>(limit)) {
            result.set_next_id(result.tasks(result.tasks_size() - 1).request().id());
            break;
        }
        if (const auto at = active_.find(id); at != active_.end()
            && at->second->finished.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            at->second->finished.get();
        *result.add_tasks() = load(directory_ / (id + ".pb"));
    }
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

void Manager::run(Active& active, Source& source, warehouse::Store& store)
{
    wire::Task task;
    {
        std::lock_guard lock(mutex_);
        task = active.task;
    }
    if (task.request().exchanges().empty())
        for (const auto* exchange : {"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"})
            if (source.supports(task.request().table(), exchange))
                task.mutable_request()->add_exchanges(exchange);
    const auto& request = task.request();
    if (request.exchanges().empty())
        throw SourceError("unsupported", "source does not provide the requested table");
    for (const auto& exchange : request.exchanges())
        if (!source.supports(request.table(), exchange))
            throw SourceError("unsupported", "source does not provide this table for " + exchange);
    publish(task, active);
    const auto stop = active.cancel.get_token();
    // Provider trading dates use Beijing's civil day, independent of host TZ.
    const auto today = std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now() + std::chrono::hours(8));
    // A provider's day so far would be stored as the whole day and never taken
    // again: other sources stop at yesterday.
    const auto first = day_of(request.begin_day()),
               last = std::min(
                   day_of(request.end_day()),
                   source.marks_partial_days() ? today : today - std::chrono::days{1});
    if (last < first)
        return;
    auto writer = store.writer();
    auto version = store.current();
    task.set_dataset_version(version.identity);
    const auto commit = [&] {
        version = writer.commit();
        warehouse::compose_research(directory_.parent_path(), research_);
        task.set_dataset_version(version.identity);
        task.set_requests(source.requests());
        publish(task, active);
    };
    if (request.table() == "minutes" || request.table() == "settlement"
        || !source.supports("calendar", request.exchanges(0))) {
        // Exchange, day and, for minutes, contract of each partition.
        std::vector<std::tuple<std::string, std::string, std::string>> plan;
        if (request.table() == "minutes" && request.contract().empty())
            for (auto day = first; day <= last; day += std::chrono::days(1))
                for (const auto& contract : source.minute_contracts(day_text(day)))
                    plan.emplace_back(
                        contract.substr(0, contract.find('/')),
                        day_text(day),
                        contract);
        else
            for (const auto& exchange : request.exchanges())
                for (auto day = first; day <= last; day += std::chrono::days(1)) {
                    const auto weekday = std::chrono::weekday(day).iso_encoding();
                    if (request.table() == "minutes" || weekday < 6)
                        plan.emplace_back(exchange, day_text(day), request.contract());
                }
        task.set_days_total(plan.size());
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
                auto key = request.table() + "/" + exchange + "/" + day;
                if (!contract.empty())
                    key += "/" + contract.substr(exchange.size() + 1);
                if (request.refresh() || !version.partitions.contains(key)) {
                    MinuteBatch batch;
                    if (request.table() == "minutes")
                        batch = source.minutes(contract, day);
                    else
                        batch.rows = request.table() == "settlement"
                            ? source.settlement(exchange, day)
                            : source.daily(exchange, day);
                    check_stop(stop);
                    if (batch.rows.empty())
                        task.set_days_empty(task.days_empty() + 1);
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
                task.set_days_done(task.days_done() + 1);
                task.set_requests(source.requests());
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
        if (request.table() == "daily" || request.table() == "calendar") {
            for (int year = int(std::chrono::year_month_day(first).year());
                year <= int(std::chrono::year_month_day(last).year());
                ++year) {
                const auto key = "calendar/" + exchange + "/" + std::to_string(year);
                if (request.refresh() || !version.partitions.contains(key)
                    || year == int(std::chrono::year_month_day(today).year()))
                    writer.put(key, source.calendar(exchange, year));
            }
        }
        if (request.table() == "daily" || request.table() == "contracts")
            writer.put("contracts/" + exchange, source.contracts(exchange));
    }
    check_stop(stop);
    commit();
    if (request.table() != "daily")
        return;
    std::vector<std::pair<std::string, std::string>> plan;
    for (const auto& exchange : request.exchanges()) {
        std::set<std::string> stored;
        if (!request.refresh())
            for (const auto& [key, part] : version.partitions)
                if (key.starts_with("daily/" + exchange + "/"))
                    stored.insert(part.days.begin(), part.days.end());
        for (int year = int(std::chrono::year_month_day(first).year());
            year <= int(std::chrono::year_month_day(last).year());
            ++year) {
            for (const auto& row :
                store.read(version, "calendar/" + exchange + "/" + std::to_string(year))) {
                const auto day = std::get<std::string>(row[1]);
                if (std::get<bool>(row[2]) && day >= request.begin_day() && day <= day_text(last)
                    && !stored.contains(day))
                    plan.emplace_back(exchange, day);
            }
        }
    }
    std::sort(plan.begin(), plan.end());
    task.set_days_total(plan.size());
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
                task.set_days_empty(task.days_empty() + 1);
            else {
                rows.insert(
                    rows.end(),
                    std::make_move_iterator(batch.begin()),
                    std::make_move_iterator(batch.end()));
                days.push_back(day);
            }
            task.set_days_done(task.days_done() + 1);
            task.set_requests(source.requests());
            publish(task, active);
        }
    } catch (...) {
        flush();
        throw;
    }
    flush();
}

} // namespace asterion::market_data::acquisition
