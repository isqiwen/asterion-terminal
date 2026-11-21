#include "manager.hpp"
#include "freshness.hpp"
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/warehouse.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/durable_file.hpp>
#include <algorithm>
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
    for (const auto* name :
        {"daily",
            "minutes",
            "settlement",
            "price_ticks",
            "calendar",
            "contracts",
            "warehouse_receipts",
            "member_positions",
            "inventory"}) {
        std::set<std::string> exchanges;
        for (const auto& provider : warehouse::sources_of(name)) {
            if (!sources_.contains(provider))
                continue;
            const auto source = sources_.at(provider)({});
            for (const auto* exchange : all_exchanges)
                if (source->supports(name, exchange)) {
                    exchanges.insert(exchange);
                }
        }
        if (!exchanges.empty()) {
            auto* item = result.add_tables();
            item->set_table(protocol::table_of(name));
            for (const auto* exchange : all_exchanges)
                if (exchanges.contains(exchange))
                    item->add_exchanges(exchange);
        }
    }
    return result;
}

Manager::Manager(
    fs::path directory,
    std::map<std::string, Factory> sources,
    std::function<void(const SourceVersions&)> finished,
    std::vector<PriceTick> verified_price_ticks)
    : directory_(std::move(directory) / "acquisition"),
      sources_(std::move(sources)),
      finished_(std::move(finished)),
      verified_price_ticks_(std::move(verified_price_ticks))
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

std::pair<warehouse::Version, warehouse::OpenDays> Manager::calendar() const
{
    for (const auto& provider : warehouse::sources_of("calendar"))
        if (const auto at = stores_.find(provider); at != stores_.end()) {
            auto version = at->second->current();
            auto open = warehouse::open_days(*at->second, version, warehouse::yesterday());
            return {std::move(version), std::move(open)};
        }
    return {};
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
        try {
            it->second->finished.get();
        } catch (...) {
            if (it->second->task.state() != wire::TASK_STATE_FAILED)
                throw;
            persist(it->second->task);
        }
        it = active_.erase(it);
    }
}

wire::AcquisitionTask Manager::submit(wire::StartAcquisition request)
{
    validate(request);
    const auto supported =
        std::ranges::any_of(warehouse::sources_of(table(request)), [&](const auto& provider) {
            return sources_.contains(provider);
        });
    if (!supported)
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
                            .submit([this, current, request](std::stop_token shutdown) {
                                std::stop_callback cancel(shutdown, [&] {
                                    current->cancel.request_stop();
                                });
                                work(*current, request);
                            })
                            .share();
    return accepted;
}

void Manager::work(Active& active, const wire::StartAcquisition& request)
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
        const auto target = table(request);
        const std::vector<std::string> steps = target == "daily"
            ? std::vector<std::string>{"calendar", "contracts", "settlement", "daily"}
            : target == "minutes"
            ? std::vector<std::string>{"calendar", "contracts", "settlement", "daily", "minutes"}
            : target == "warehouse_receipts" || target == "member_positions"
                || target == "inventory"
            ? std::vector<std::string>{"calendar", "contracts", target}
            : std::vector<std::string>{target};
        // Provider trading dates use Beijing's civil day, independent of host TZ.
        const auto today = std::chrono::floor<std::chrono::days>(
            std::chrono::system_clock::now() + std::chrono::hours(8));
        std::map<std::string, std::set<std::string>> planned;
        bool dated_planned = false;
        std::set<std::string> unavailable;
        std::optional<wire::AcquisitionTask> failure;
        bool offered = false;
        for (const auto& step : steps) {
            check_stop(active.cancel.get_token());
            if (!dated_planned && step != "calendar" && step != "contracts") {
                dated_planned = true;
                const auto [held_calendar, open] = calendar();
                std::uint64_t total = 0;
                for (const auto& dated : steps)
                    if (dated != "calendar" && dated != "contracts") {
                        planned[dated] = planned_keys(
                            active,
                            dated,
                            whole,
                            request.refetch(),
                            unavailable,
                            held_calendar,
                            open,
                            today);
                        total += planned[dated].size();
                    }
                if (total > 0) {
                    std::lock_guard lock(mutex_);
                    active.task.set_items(total);
                }
            }
            const bool primary = step == target;
            const bool step_offered = acquire_table(
                active,
                step,
                whole,
                request.refetch(),
                primary,
                planned[step],
                today,
                unavailable,
                failure);
            if (primary)
                offered = step_offered;
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
    active.sources.clear();
    if (finished_)
        finished_(active.versions);
    // The source may have committed data immediately before a status write
    // failed. The warehouse remains authoritative; never roll it back.
    try {
        publish(task, active);
    } catch (const std::exception& error) {
        task.set_state(wire::TASK_STATE_FAILED);
        if (task.has_error())
            task.mutable_error()->set_message(
                task.error().message() + "; task state was not saved: " + error.what());
        else
            *task.mutable_error() = protocol::error(error);
        std::lock_guard lock(mutex_);
        active.task = std::move(task);
        throw;
    }
}

std::set<std::string> Manager::planned_keys(
    Active& active,
    const std::string& name,
    const Scope& whole,
    bool refetch,
    const std::set<std::string>& unavailable,
    const warehouse::Version& held_calendar,
    const warehouse::OpenDays& open,
    std::chrono::sys_days today)
{
    const auto first = whole.first, last = std::min(whole.last, today - std::chrono::days{1});
    if (last < first)
        return {};
    std::map<std::string, warehouse::Version> current_versions;
    std::map<std::string, const warehouse::Version*> lead_versions;
    for (const auto& provider : warehouse::sources_of(name)) {
        if (!sources_.contains(provider) || unavailable.contains(provider))
            continue;
        auto& current = source(active, provider);
        for (const auto& exchange : whole.exchanges)
            if (!lead_versions.contains(exchange) && current.supports(name, exchange)) {
                auto [it, _] =
                    current_versions.try_emplace(provider, stores_.at(provider)->current());
                lead_versions[exchange] = &it->second;
            }
    }
    if (lead_versions.empty())
        return {};
    const auto answered =
        refetch ? std::set<std::string>{} : warehouse::empty_keys(directory_.parent_path(), name);
    const auto trading = [&](const std::string& exchange, std::chrono::sys_days day) {
        const auto text = day_text(day);
        if (!held_calendar.partitions.contains("calendar/" + exchange + "/" + text.substr(0, 4)))
            return std::chrono::weekday(day).iso_encoding() < 6;
        const auto days = open.find(exchange);
        return days != open.end()
            && std::binary_search(days->second.begin(), days->second.end(), text);
    };
    std::set<std::string> result;
    const auto consider =
        [&](const std::string& exchange, const std::string& day, const std::string& contract) {
            const auto lead = lead_versions.find(exchange);
            if (lead == lead_versions.end())
                return;
            auto key = name + "/" + exchange + "/" + day;
            if (!contract.empty())
                key += "/" + contract.substr(exchange.size() + 1);
            if (refetch || (!lead->second->partitions.contains(key) && !answered.contains(key)))
                result.insert(std::move(key));
        };
    if (name == "minutes") {
        for (const auto& [contract, from, to] : whole.contracts)
            for (auto day = from; day <= std::min(to, last); day += std::chrono::days(1))
                if (const auto exchange = contract.substr(0, contract.find('/'));
                    trading(exchange, day))
                    consider(exchange, day_text(day), contract);
    } else {
        for (auto day = first; day <= last; day += std::chrono::days(1))
            for (const auto& exchange : whole.exchanges)
                if (trading(exchange, day))
                    consider(exchange, day_text(day), "");
    }
    return result;
}

bool Manager::acquire_table(
    Active& active,
    const std::string& name,
    const Scope& whole,
    bool refetch,
    bool primary,
    const std::set<std::string>& planned,
    std::chrono::sys_days today,
    std::set<std::string>& unavailable,
    std::optional<wire::AcquisitionTask>& failure)
{
    const auto copy = [&] {
        std::lock_guard lock(mutex_);
        return active.task;
    };
    std::map<std::string, std::string> last_provider;
    for (const auto& provider : warehouse::sources_of(name)) {
        if (!sources_.contains(provider))
            continue;
        auto& current = source(active, provider);
        for (const auto& exchange : whole.exchanges)
            if (current.supports(name, exchange))
                last_provider[exchange] = provider;
    }
    // A source that fails leaves the next ones to fill what it lacks.
    std::set<std::string> covered, empty, counted;
    const auto answered = warehouse::empty_keys(directory_.parent_path(), name);
    bool offered = false;
    bool step_failed = false;
    for (const auto& provider : warehouse::sources_of(name)) {
        if (!sources_.contains(provider))
            continue;
        auto& current = source(active, provider);
        auto part = whole;
        std::erase_if(part.exchanges, [&](const auto& exchange) {
            return !current.supports(name, exchange);
        });
        std::erase_if(part.contracts, [&](const auto& contract) {
            const auto& id = std::get<0>(contract);
            return !current.supports(name, id.substr(0, id.find('/')));
        });
        auto& store = *stores_.at(provider);
        const bool any = name == "minutes" ? !part.contracts.empty() : !part.exchanges.empty();
        offered = offered || any;
        if (any) {
            if (unavailable.contains(provider))
                step_failed = true;
            else
                try {
                    run(active,
                        name,
                        refetch,
                        primary,
                        current,
                        store,
                        part,
                        planned,
                        counted,
                        last_provider,
                        today,
                        covered,
                        answered,
                        empty);
                } catch (const SourceError& error) {
                    if (error.code == "cancelled")
                        throw;
                    step_failed = true;
                    if (error.code == "unavailable" || error.code == "invalid_credential"
                        || error.code == "permission" || error.code == "rate_limit")
                        unavailable.insert(provider);
                    auto failed = copy();
                    failed.mutable_error()->set_code(
                        protocol::error_code(
                            parse_error_code(error.code).value_or(ErrorCode::operation_failed)));
                    failed.mutable_error()->set_message(provider + ": " + error.what());
                    if (!failure)
                        failure = std::move(failed);
                } catch (const std::exception& error) {
                    step_failed = true;
                    log::error(
                        "acquisition {} from {} failed: {}",
                        copy().request().id(),
                        provider,
                        error.what());
                    auto failed = copy();
                    *failed.mutable_error() = protocol::error(error);
                    if (!failure)
                        failure = std::move(failed);
                }
        }
        for (const auto& [key, stored] : store.current().partitions)
            covered.insert(key);
    }
    // A day every source still answers empty after two more of its
    // exchange's trading days will not come later; a provider may publish
    // late, or after a holiday.
    if (!step_failed && name != "calendar" && name != "contracts") {
        const auto open = calendar().second;
        auto record = answered;
        for (const auto& key : empty) {
            const auto exchange = key.find('/') + 1, day = key.find('/', exchange) + 1;
            const auto days = open.find(key.substr(exchange, day - exchange - 1));
            if (!covered.contains(key) && days != open.end()
                && days->second.end()
                        - std::upper_bound(
                            days->second.begin(),
                            days->second.end(),
                            key.substr(day, 10))
                    >= 2)
                record.insert(key);
        }
        std::erase_if(record, [&](const auto& key) {
            return covered.contains(key);
        });
        if (record != answered) {
            std::string text;
            for (const auto& key : record)
                text += key + '\n';
            replace_file_durably(warehouse::empty_file(directory_.parent_path(), name), text);
        }
    }
    return offered;
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
        if (pending(at->second->task)
            && at->second->finished.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
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
    std::vector<wire::AcquisitionTask> tasks;
    for (const auto& [id, active] : active_) {
        if (pending(active->task)
            && active->finished.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            active->finished.get();
        tasks.push_back(active->task);
    }
    for (const auto& entry : fs::directory_iterator(directory_)) {
        if (entry.path().extension() != ".pb" || active_.contains(entry.path().stem().string()))
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

bool Manager::busy(wire::Table table) const
{
    std::lock_guard lock(mutex_);
    return std::any_of(active_.begin(), active_.end(), [&](const auto& entry) {
        return entry.second->task.request().table() == table && pending(entry.second->task);
    });
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

Source& Manager::source(Active& active, const std::string& provider)
{
    auto& instance = active.sources[provider];
    if (!instance)
        instance = sources_.at(provider)(active.cancel.get_token());
    return *instance;
}

std::vector<Contract> Manager::listed(Active& active, const std::string& exchange)
{
    // Refreshed on each use, so a product includes its newly listed contracts.
    for (const auto& provider : warehouse::sources_of("contracts")) {
        if (!sources_.contains(provider))
            continue;
        auto& current = source(active, provider);
        if (!current.supports("contracts", exchange))
            continue;
        const auto before = current.requests();
        auto contracts = current.contracts(exchange);
        auto writer = stores_.at(provider)->writer();
        writer.put("contracts/" + exchange, warehouse::rows(contracts));
        active.versions[provider] = writer.commit().identity;
        wire::AcquisitionTask task;
        {
            std::lock_guard lock(mutex_);
            task = active.task;
        }
        task.set_requests_sent(task.requests_sent() + (current.requests() - before));
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
        for (auto& [exchange, contracts] : exchanges) {
            contracts = listed(active, exchange);
            result.listed_exchanges.insert(exchange);
        }
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
    for (const auto* exchange : all_exchanges)
        if (named.contains(exchange))
            result.exchanges.push_back(exchange);
    result.first = std::ranges::min(spans | std::views::values | std::views::keys);
    result.last = std::ranges::max(spans | std::views::values | std::views::elements<1>);
    return result;
}

void Manager::run(
    Active& active,
    const std::string& name,
    bool refetch,
    bool primary,
    Source& source,
    warehouse::Store& store,
    const Scope& scope,
    const std::set<std::string>& planned,
    std::set<std::string>& counted,
    const std::map<std::string, std::string>& last_provider,
    std::chrono::sys_days today,
    const std::set<std::string>& covered,
    const std::set<std::string>& answered,
    std::set<std::string>& empty)
{
    wire::AcquisitionTask task;
    {
        std::lock_guard lock(mutex_);
        task = active.task;
    }
    const auto previous_requests = task.requests_sent();
    const auto source_start = source.requests();
    const auto stop = active.cancel.get_token();
    // A provider's day so far would be stored as the whole day and never taken
    // again: other sources stop at yesterday.
    const auto first = scope.first, last = std::min(scope.last, today - std::chrono::days{1});
    if (last < first)
        return;
    auto writer = store.writer();
    auto version = store.current();
    const auto commit = [&] {
        version = writer.commit();
        active.versions[store.provider()] = version.identity;
        task.set_requests_sent(previous_requests + (source.requests() - source_start));
        publish(task, active);
    };
    if (name == "calendar" || name == "contracts") {
        bool wrote = false;
        for (const auto& exchange : scope.exchanges) {
            check_stop(stop);
            if (name == "calendar") {
                for (int year = int(std::chrono::year_month_day(first).year());
                    year <= int(std::chrono::year_month_day(last).year());
                    ++year) {
                    const auto key = "calendar/" + exchange + "/" + std::to_string(year);
                    if (refetch || !version.partitions.contains(key)
                        || year == int(std::chrono::year_month_day(today).year())) {
                        writer.put(key, warehouse::rows(source.calendar(exchange, year)));
                        wrote = true;
                    }
                }
            } else if (refetch || !scope.listed_exchanges.contains(exchange)) {
                writer.put("contracts/" + exchange, warehouse::rows(source.contracts(exchange)));
                wrote = true;
            }
        }
        check_stop(stop);
        if (wrote)
            commit();
        return;
    }
    // An exchange's open days by its calendar for that year; weekdays without one.
    const auto [held, open] = calendar();
    const auto trading = [&](const std::string& exchange, std::chrono::sys_days day) {
        const auto text = day_text(day);
        if (!held.partitions.contains("calendar/" + exchange + "/" + text.substr(0, 4)))
            return std::chrono::weekday(day).iso_encoding() < 6;
        const auto days = open.find(exchange);
        return days != open.end()
            && std::binary_search(days->second.begin(), days->second.end(), text);
    };
    // Exchange, day and, for minutes, contract of each partition.
    std::vector<std::tuple<std::string, std::string, std::string>> plan;
    if (name == "minutes") {
        for (const auto& [contract, from, to] : scope.contracts)
            for (auto day = from; day <= std::min(to, last); day += std::chrono::days(1))
                if (const auto exchange = contract.substr(0, contract.find('/'));
                    trading(exchange, day))
                    plan.emplace_back(exchange, day_text(day), contract);
    } else {
        // Day-outer so multi-exchange daily reports are fetched once per day.
        for (auto day = first; day <= last; day += std::chrono::days(1))
            for (const auto& exchange : scope.exchanges)
                if (trading(exchange, day))
                    plan.emplace_back(exchange, day_text(day), "");
    }
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
            auto key = name + "/" + exchange + "/" + day;
            if (!contract.empty())
                key += "/" + contract.substr(exchange.size() + 1);
            const bool need_fetch = !covered.contains(key)
                && (refetch || (!version.partitions.contains(key) && !answered.contains(key)));
            bool resolved = !need_fetch;
            if (need_fetch) {
                using warehouse::rows;
                if (name == "price_ticks" && !verified_price_ticks_.empty()) {
                    const auto ticks = source.price_ticks(exchange, day);
                    check_stop(stop);
                    if (ticks.empty()) {
                        empty.insert(key);
                        if (const auto last_it = last_provider.find(exchange);
                            last_it != last_provider.end() && last_it->second == store.provider()) {
                            resolved = true;
                            if (primary)
                                task.set_items_empty(task.items_empty() + 1);
                        }
                    } else {
                        warehouse::verify_price_ticks(verified_price_ticks_, ticks, day);
                        resolved = true;
                    }
                } else {
                    const auto fetched = name == "minutes" ? rows(source.minutes(contract, day))
                        : name == "settlement"             ? rows(source.settlement(exchange, day))
                        : name == "price_ticks"            ? rows(source.price_ticks(exchange, day))
                        : name == "warehouse_receipts"
                        ? rows(source.warehouse_receipts(exchange, day))
                        : name == "member_positions" ? rows(source.member_positions(exchange, day))
                        : name == "inventory"        ? rows(source.inventory(exchange, day))
                                                     : rows(source.daily(exchange, day));
                    check_stop(stop);
                    if (fetched.empty()) {
                        empty.insert(key);
                        if (const auto last_it = last_provider.find(exchange);
                            last_it != last_provider.end() && last_it->second == store.provider()) {
                            resolved = true;
                            if (primary)
                                task.set_items_empty(task.items_empty() + 1);
                        }
                    } else {
                        writer.put(key, fetched, {day});
                        ++unpublished;
                        resolved = true;
                    }
                }
            }
            bool progressed = false;
            if (resolved && planned.contains(key) && counted.insert(key).second) {
                task.set_items_done(task.items_done() + 1);
                progressed = true;
            }
            if (unpublished >= 200
                || std::chrono::steady_clock::now() - published_at >= std::chrono::minutes(5))
                publish_batch();
            task.set_requests_sent(previous_requests + (source.requests() - source_start));
            if (need_fetch || progressed)
                publish(task, active);
        }
    } catch (...) {
        publish_batch();
        throw;
    }
    publish_batch();
}

} // namespace asterion::warehouse::acquisition
