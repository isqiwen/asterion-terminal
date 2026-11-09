#include "manager.hpp"
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/warehouse.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/durable_file.hpp>
#include <fstream>
#include <ranges>
#include <regex>
#include <set>

namespace asterion::warehouse::acquisition {
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

// EXCHANGE/product, a whole product, rather than one of its contracts.
bool product(const std::string& instrument)
{
    return std::count(instrument.begin(), instrument.end(), '/') == 1;
}

void validate(const wire::StartAcquisition& request)
{
    require_id(request.id());
    for (const auto* day : {&request.first_day(), &request.last_day()})
        if (!day->empty() && day_of(*day) < day_of("1990-01-01"))
            throw std::invalid_argument("acquisition days start in 1990");
    if (!request.first_day().empty() && !request.last_day().empty()
        && request.first_day() > request.last_day())
        throw std::invalid_argument("acquisition ends before it starts");
    std::set<std::string> seen;
    for (const auto& exchange : request.exchanges()) {
        require_exchange(exchange);
        if (!seen.insert(exchange).second)
            throw std::invalid_argument("duplicate acquisition exchange");
    }
    for (const auto& instrument : request.instruments()) {
        static const std::regex products("[A-Z]+/[a-z]{1,3}");
        if (!product(instrument))
            require_contract(instrument);
        else if (!std::regex_match(instrument, products))
            throw std::invalid_argument("a product must be EXCHANGE/product");
        else
            require_exchange(instrument.substr(0, instrument.find('/')));
        if (!seen.insert(instrument).second)
            throw std::invalid_argument("duplicate acquisition instrument");
    }
    if (request.instruments().empty()) {
        if (request.first_day().empty() || request.last_day().empty())
            throw std::invalid_argument("an exchange acquisition needs its first and last day");
        if (table(request) == "minutes")
            throw std::invalid_argument("a minute acquisition names its products or contracts");
    } else if (!request.exchanges().empty())
        throw std::invalid_argument("an acquisition names exchanges or instruments, not both");
    else if (table(request) == "calendar" || table(request) == "contracts")
        throw std::invalid_argument("calendars and contracts are acquired by exchange");
}

constexpr const char* all_exchanges[] = {"SHFE", "INE", "DCE", "CZCE", "CFFEX", "GFEX"};

} // namespace

wire::AcquisitionOptions Manager::options() const
{
    wire::AcquisitionOptions result;
    std::set<std::string> exchanges;
    for (const auto* name :
        {"daily", "minutes", "settlement", "price_ticks", "calendar", "contracts"}) {
        bool offered = false;
        for (const auto& provider : warehouse::sources_of(name)) {
            if (!sources_.contains(provider))
                continue;
            const auto source = sources_.at(provider)({});
            for (const auto* exchange : all_exchanges)
                if (source->supports(name, exchange)) {
                    exchanges.insert(exchange);
                    offered = true;
                }
        }
        if (offered)
            result.add_tables(protocol::table_of(name));
    }
    for (const auto& exchange : exchanges)
        result.add_exchanges(exchange);
    return result;
}

Manager::Manager(
    fs::path directory,
    std::map<std::string, Factory> sources,
    std::function<void()> finished)
    : directory_(std::move(directory) / "acquisition"),
      sources_(std::move(sources)),
      finished_(std::move(finished))
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
                task.mutable_error()->set_message("Warehouse stopped before acquisition completed");
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
        if (it->second->finished.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
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
    std::vector<std::string> providers;
    for (const auto& provider : warehouse::sources_of(table(request)))
        if (sources_.contains(provider))
            providers.push_back(provider);
    if (providers.empty())
        throw std::invalid_argument("no source provides this table");
    std::lock_guard lock(mutex_);
    if (stopped_)
        throw Error(ErrorCode::unavailable, "acquisition is stopped");
    reap();
    if (fs::exists(directory_ / (request.id() + ".pb")))
        throw Error(ErrorCode::conflict, "acquisition task id already exists");
    if (active_.size() >= 16)
        throw Error(ErrorCode::resource_exhausted, "acquisition queue is full");
    auto active = std::make_unique<Active>();
    *active->task.mutable_request() = request;
    active->task.set_state(wire::TASK_STATE_QUEUED);
    active->task.set_started_ms(now_ms());
    persist(active->task);
    const auto accepted = active->task;
    auto* current = active.get();
    active_.emplace(request.id(), std::move(active));
    current->finished = workers_
                            .submit([this, current, request, providers](std::stop_token shutdown) {
                                std::stop_callback cancel(shutdown, [&] {
                                    current->cancel.request_stop();
                                });
                                work(*current, request, providers);
                            })
                            .share();
    return accepted;
}

void Manager::work(
    Active& active,
    const wire::StartAcquisition& request,
    const std::vector<std::string>& providers)
{
    const auto copy = [&] {
        std::lock_guard lock(mutex_);
        return active.task;
    };
    auto task = copy();
    try {
        check_stop(active.cancel.get_token());
        task.set_state(wire::TASK_STATE_RUNNING);
        publish(task, active);
        const auto whole = scope(active, request);
        // A source that fails leaves the next ones to fill what it lacks.
        std::set<std::string> covered;
        std::optional<wire::AcquisitionTask> failure;
        bool offered = false;
        for (const auto& provider : providers) {
            auto source = sources_.at(provider)(active.cancel.get_token());
            auto part = whole;
            std::erase_if(part.exchanges, [&](const auto& exchange) {
                return !source->supports(table(request), exchange);
            });
            std::erase_if(part.contracts, [&](const auto& contract) {
                const auto& name = std::get<0>(contract);
                return !source->supports(table(request), name.substr(0, name.find('/')));
            });
            auto& store = *stores_.at(provider);
            const bool any =
                table(request) == "minutes" ? !part.contracts.empty() : !part.exchanges.empty();
            offered = offered || any;
            if (any)
                try {
                    run(active, *source, store, part, covered);
                } catch (const SourceError& error) {
                    if (error.code == "cancelled")
                        throw;
                    auto failed = copy();
                    failed.mutable_error()->set_code(
                        protocol::error_code(
                            parse_error_code(error.code).value_or(ErrorCode::operation_failed)));
                    failed.mutable_error()->set_message(provider + ": " + error.what());
                    if (!failure)
                        failure = std::move(failed);
                } catch (const std::exception& error) {
                    log::error(
                        "acquisition {} from {} failed: {}",
                        request.id(),
                        provider,
                        error.what());
                    auto failed = copy();
                    *failed.mutable_error() = protocol::error(error);
                    if (!failure)
                        failure = std::move(failed);
                }
            for (const auto& [key, part] : store.current().partitions)
                covered.insert(key);
        }
        task = copy();
        if (failure) {
            *task.mutable_error() = failure->error();
            task.set_state(wire::TASK_STATE_FAILED);
        } else {
            check_stop(active.cancel.get_token());
            if (!offered)
                throw SourceError(
                    "unsupported",
                    "no source provides this table for these exchanges");
            task.set_state(wire::TASK_STATE_SUCCEEDED);
        }
    } catch (const SourceError& error) {
        task = copy();
        task.set_state(
            error.code == "cancelled" ? wire::TASK_STATE_CANCELLED : wire::TASK_STATE_FAILED);
        task.mutable_error()->set_code(
            protocol::error_code(
                error.code == "unsupported"
                    ? ErrorCode::invalid_request
                    : parse_error_code(error.code).value_or(ErrorCode::operation_failed)));
        task.mutable_error()->set_message(error.what());
    } catch (const std::exception& error) {
        task = copy();
        task.set_state(wire::TASK_STATE_FAILED);
        *task.mutable_error() = protocol::error(error);
    }
    task.set_finished_ms(now_ms());
    if (finished_)
        finished_();
    // The source may have committed data immediately before a status write
    // failed. The warehouse remains authoritative; never roll it back.
    publish(task, active);
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
    std::lock_guard lock(mutex_);
    for (const auto& [id, active] : active_)
        if (active->finished.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            active->finished.get();
    std::vector<wire::AcquisitionTask> tasks;
    for (const auto& entry : fs::directory_iterator(directory_)) {
        if (entry.path().extension() != ".pb")
            continue;
        try {
            tasks.push_back(load(entry.path()));
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

std::vector<Contract> Manager::listed(Active& active, const std::string& exchange)
{
    // Refreshed on each use, so a product includes its newly listed contracts.
    for (const auto& provider : warehouse::sources_of("contracts")) {
        if (!sources_.contains(provider))
            continue;
        const auto source = sources_.at(provider)(active.cancel.get_token());
        if (!source->supports("contracts", exchange))
            continue;
        auto contracts = source->contracts(exchange);
        auto writer = stores_.at(provider)->writer();
        writer.put("contracts/" + exchange, warehouse::rows(contracts));
        writer.commit();
        wire::AcquisitionTask task;
        {
            std::lock_guard lock(mutex_);
            task = active.task;
        }
        task.set_requests_sent(task.requests_sent() + source->requests());
        publish(task, active);
        return contracts;
    }
    throw SourceError("unavailable", "no source lists the contracts of " + exchange);
}

Manager::Scope Manager::scope(Active& active, const wire::StartAcquisition& request)
{
    Scope result;
    if (request.instruments().empty()) {
        for (const auto* exchange : all_exchanges) {
            const auto& named = request.exchanges();
            if (named.empty() || std::find(named.begin(), named.end(), exchange) != named.end())
                result.exchanges.push_back(exchange);
        }
        result.first = day_of(request.first_day());
        result.last = day_of(request.last_day());
        return result;
    }
    // A contract with both days given needs no listing; products and open
    // ends take each contract's listed life.
    const bool lookup = request.first_day().empty() || request.last_day().empty()
        || std::any_of(request.instruments().begin(), request.instruments().end(), product);
    std::map<std::string, std::vector<Contract>> exchanges;
    for (const auto& instrument : request.instruments())
        exchanges[instrument.substr(0, instrument.find('/'))];
    if (lookup)
        for (auto& [exchange, contracts] : exchanges)
            contracts = listed(active, exchange);
    std::map<std::string, std::pair<std::chrono::sys_days, std::chrono::sys_days>> spans;
    const auto add = [&](const std::string& contract, std::string first, std::string last) {
        if (!request.first_day().empty())
            first = std::max(first, request.first_day());
        if (!request.last_day().empty())
            last = std::min(last, request.last_day());
        if (first <= last)
            spans[contract] = {day_of(first), day_of(last)};
    };
    for (const auto& instrument : request.instruments()) {
        bool found = false;
        for (const auto& c : exchanges.at(instrument.substr(0, instrument.find('/'))))
            if (product(instrument) ? c.exchange + "/" + c.product == instrument
                                    : c.contract == instrument) {
                add(c.contract, c.list_date, c.delist_date);
                found = true;
            }
        if (found)
            continue;
        if (lookup)
            throw SourceError("not_found", instrument + " is not in the contracts table");
        add(instrument, request.first_day(), request.last_day());
    }
    if (spans.empty())
        throw SourceError("not_found", "no contract of these instruments is listed in these days");
    std::set<std::string> named;
    for (const auto& [contract, days] : spans) {
        named.insert(contract.substr(0, contract.find('/')));
        result.contracts.emplace_back(contract, days.first, days.second);
    }
    result.exchanges.assign(named.begin(), named.end());
    result.first = std::ranges::min(spans | std::views::values | std::views::keys);
    result.last = std::ranges::max(spans | std::views::values | std::views::elements<1>);
    return result;
}

void Manager::run(
    Active& active,
    Source& source,
    warehouse::Store& store,
    const Scope& scope,
    const std::set<std::string>& covered)
{
    wire::AcquisitionTask task;
    {
        std::lock_guard lock(mutex_);
        task = active.task;
    }
    const auto& request = task.request();
    const auto previous_requests = task.requests_sent();
    const auto stop = active.cancel.get_token();
    // Provider trading dates use Beijing's civil day, independent of host TZ.
    const auto today = std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now() + std::chrono::hours(8));
    // A provider's day so far would be stored as the whole day and never taken
    // again: other sources stop at yesterday.
    const auto first = scope.first, last = std::min(scope.last, today - std::chrono::days{1});
    if (last < first)
        return;
    auto writer = store.writer();
    auto version = store.current();
    const auto commit = [&] {
        version = writer.commit();
        task.set_requests_sent(previous_requests + source.requests());
        publish(task, active);
    };
    // Exchange, day and, for minutes, contract of each partition.
    std::vector<std::tuple<std::string, std::string, std::string>> plan;
    if (table(request) == "minutes") {
        for (const auto& [contract, from, to] : scope.contracts)
            for (auto day = from; day <= std::min(to, last); day += std::chrono::days(1))
                plan.emplace_back(contract.substr(0, contract.find('/')), day_text(day), contract);
    } else if (table(request) == "calendar" || table(request) == "contracts"
        || (table(request) == "daily" && source.supports("calendar", scope.exchanges[0]))) {
        for (const auto& exchange : scope.exchanges) {
            check_stop(stop);
            if (table(request) != "contracts")
                for (int year = int(std::chrono::year_month_day(first).year());
                    year <= int(std::chrono::year_month_day(last).year());
                    ++year) {
                    const auto key = "calendar/" + exchange + "/" + std::to_string(year);
                    if (request.refetch() || !version.partitions.contains(key)
                        || year == int(std::chrono::year_month_day(today).year()))
                        writer.put(key, warehouse::rows(source.calendar(exchange, year)));
                }
            if (table(request) != "calendar")
                writer.put("contracts/" + exchange, warehouse::rows(source.contracts(exchange)));
        }
        check_stop(stop);
        commit();
        if (table(request) != "daily")
            return;
        for (const auto& exchange : scope.exchanges)
            for (int year = int(std::chrono::year_month_day(first).year());
                year <= int(std::chrono::year_month_day(last).year());
                ++year)
                for (const auto& row :
                    store.read(version, "calendar/" + exchange + "/" + std::to_string(year))) {
                    const auto day = warehouse::read_calendar_day(row);
                    if (day.open && day.day >= day_text(first) && day.day <= day_text(last))
                        plan.emplace_back(exchange, day.day, "");
                }
        std::sort(plan.begin(), plan.end());
    } else {
        for (auto day = first; day <= last; day += std::chrono::days(1))
            for (const auto& exchange : scope.exchanges)
                if (std::chrono::weekday(day).iso_encoding() < 6)
                    plan.emplace_back(exchange, day_text(day), "");
    }
    task.set_items(task.items() + plan.size());
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
            if (!covered.contains(key)
                && (request.refetch() || !version.partitions.contains(key))) {
                using warehouse::rows;
                const auto fetched = table(request) == "minutes"
                    ? rows(source.minutes(contract, day))
                    : table(request) == "settlement"  ? rows(source.settlement(exchange, day))
                    : table(request) == "price_ticks" ? rows(source.price_ticks(exchange, day))
                                                      : rows(source.daily(exchange, day));
                check_stop(stop);
                if (fetched.empty())
                    task.set_items_empty(task.items_empty() + 1);
                else {
                    writer.put(key, fetched, {day});
                    ++unpublished;
                }
            }
            if (unpublished >= 200
                || std::chrono::steady_clock::now() - published_at >= std::chrono::minutes(5))
                publish_batch();
            task.set_items_done(task.items_done() + 1);
            task.set_requests_sent(previous_requests + source.requests());
            publish(task, active);
        }
    } catch (...) {
        publish_batch();
        throw;
    }
    publish_batch();
}

} // namespace asterion::warehouse::acquisition
