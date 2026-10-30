#include "account_journal.hpp"
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>

namespace asterion::trading {
namespace {

// The order a record removes from the unconfirmed set without a broker report.
std::string_view excluded_order(const JournalRecord& record)
{
    if (const auto* unsent = std::get_if<OrderNotSent>(&record))
        return unsent->order_id;
    if (const auto* command = std::get_if<CommandRecord>(&record))
        if (const auto* resolve = command->request.as<ResolveOrder>())
            return resolve->order_id;
    return {};
}

struct Page
{
    std::optional<Json> header;
    std::vector<JournalRecord> records;
};

Page read_page(
    const SqliteJournal& journal,
    std::uint64_t first,
    std::uint64_t total,
    std::uint64_t& submitted)
{
    const auto mismatch = [] {
        return std::invalid_argument("trading order index does not match its journal record");
    };
    Page page;
    auto sequence = first;
    for (auto& stored : journal.read(first)) {
        if (sequence == 0) {
            page.header = std::move(stored);
            ++sequence;
            continue;
        }
        auto record = parse_record(stored);
        if (const auto* command = std::get_if<CommandRecord>(&record)) {
            if (journal.command_sequence(command->request.id) != sequence)
                throw std::invalid_argument(
                    "trading command index does not match its journal record");
            if (const auto* submit = command->request.as<SubmitOrder>()) {
                ++submitted;
                const auto index = journal.order_index(submit->order.id);
                if (!index || index->sequence != sequence
                    || index->trading_day != command->trading_day
                    || index->broker_key != command->broker_key)
                    throw mismatch();
                if (index->excluded_by
                    && excluded_order(parse_record(journal.record(index->excluded_by)))
                        != submit->order.id)
                    throw mismatch();
            }
        }
        if (const auto excluded = excluded_order(record); !excluded.empty()) {
            const auto index = journal.order_index(excluded);
            if (!index || index->excluded_by != sequence)
                throw mismatch();
        }
        page.records.push_back(std::move(record));
        ++sequence;
    }
    // Unique identities plus per-record checks and equal cardinality rule out
    // an extra index row that could otherwise claim an unrelated broker report.
    if (sequence == total && journal.order_identity_count() != submitted)
        throw mismatch();
    return page;
}

} // namespace

AccountJournal::AccountJournal(
    std::filesystem::path directory,
    std::function<void(const Json&)> validate,
    Post post,
    Progress& progress)
    : directory_(directory),
      progress_(progress),
      post_(std::move(post))
{
    std::promise<void> started;
    auto ready = started.get_future();
    thread_ = std::jthread([this,
                               directory = std::move(directory),
                               validate = std::move(validate),
                               started = std::move(started)]() mutable {
        progress_.begin();
        std::unique_ptr<FileLock> directory_owner;
        std::unique_ptr<SqliteJournal> journal;
        try {
            if (!directory.is_absolute() || std::filesystem::is_symlink(directory)
                || !std::filesystem::is_directory(directory))
                throw std::invalid_argument("choose an existing trading record directory");
            // The lease alone lives in owner/, beside the business files.
            const auto owner = directory / "owner";
            if (std::filesystem::is_symlink(owner))
                throw std::invalid_argument("invalid journal sidecar directory");
            const auto lease = owner / ownership_file;
            if (std::filesystem::exists(lease) || std::filesystem::is_symlink(lease)) {
                // A stopped cut must reject this writer before normal SQLite I/O
                // could create WAL coordination files inside its inspected directory.
                directory_owner = std::make_unique<FileLock>(
                    owner,
                    ownership_file,
                    FileLock::Access::exclusive_existing);
                if (std::filesystem::file_size(lease))
                    throw std::invalid_argument("invalid account journal ownership lease");
            } else {
                // Existing business evidence is validated before adding a new empty
                // coordination slot. SqliteJournal rechecks after ownership is held.
                if (std::filesystem::exists(directory / "journal.sqlite")) {
                    const auto header = read_journal_header(directory, sidecars);
                    if (validate)
                        validate(header);
                } else
                    check_journal_directory(directory, sidecars);
                create_directories_durably(owner);
                directory_owner = std::make_unique<FileLock>(owner, ownership_file);
            }
            journal = std::make_unique<SqliteJournal>(directory, sidecars, std::move(validate));
            journal->start();
            initial_capacity_ = journal->capacity();
        } catch (...) {
            progress_.finish();
            started.set_exception(std::current_exception());
            return;
        }
        progress_.finish();
        started.set_value();
        while (auto job = jobs_.wait_pop()) {
            progress_.begin();
            (*job)(*journal);
            progress_.finish();
        }
    });
    ready.get();
}

AccountJournal::~AccountJournal()
{
    jobs_.close();
    thread_.join();
}

void AccountJournal::replay(
    const std::function<void(const Json&)>& header,
    const std::function<void(const JournalRecord&)>& apply)
{
    std::uint64_t submitted = 0;
    for (std::uint64_t first = 0; first < initial_capacity_.total_records;) {
        auto task = std::make_shared<std::packaged_task<Page(SqliteJournal&)>>(
            [first, &submitted, total = initial_capacity_.total_records](SqliteJournal& journal) {
                return read_page(journal, first, total, submitted);
            });
        auto result = task->get_future();
        if (!jobs_.try_push([task](SqliteJournal& journal) {
                (*task)(journal);
            }))
            throw std::logic_error("account journal already has a pending write");
        const auto page = result.get();
        if (page.header)
            header(*page.header);
        for (const auto& record : page.records)
            apply(record);
        first += page.records.size() + (page.header ? 1 : 0);
    }
}

std::vector<std::filesystem::path> AccountJournal::inspect_backup(
    const std::filesystem::path& directory,
    const std::function<void(const Json&)>& header,
    const std::function<void(const JournalRecord&)>& apply,
    std::stop_token stop)
{
    return SqliteJournal::inspect_backup(
        directory,
        sidecars,
        [&](const SqliteJournal& journal, const Json&) {
            std::uint64_t submitted = 0;
            const auto total = journal.capacity().total_records;
            for (std::uint64_t first = 0; first < total;) {
                if (stop.stop_requested())
                    throw Error(ErrorCode::cancelled, "trading journal inspection cancelled");
                const auto page = read_page(journal, first, total, submitted);
                if (page.header)
                    header(*page.header);
                for (const auto& record : page.records)
                    apply(record);
                first += page.records.size() + (page.header ? 1 : 0);
            }
        },
        stop);
}

SqliteJournal::Capacity AccountJournal::initialize(Json header)
{
    auto task = std::make_shared<std::packaged_task<SqliteJournal::Capacity(SqliteJournal&)>>(
        [header = std::move(header)](SqliteJournal& journal) {
            journal.append(header);
            return journal.capacity();
        });
    auto result = task->get_future();
    if (!jobs_.try_push([task](SqliteJournal& journal) {
            (*task)(journal);
        }))
        throw std::logic_error("account journal already has a pending write");
    return result.get();
}

void AccountJournal::Write::await_suspend(std::coroutine_handle<> command)
{
    const auto trace = std::string(current_trace_id());
    if (!owner.jobs_.try_push([this, command, trace](SqliteJournal& journal) {
            TraceScope context(trace);
            try {
                result = operation(journal);
            } catch (...) {
                error = std::current_exception();
            }
            // The suspended command owns this awaiter. Posting publishes its result;
            // the writer must not touch it after the account can resume.
            owner.post_([command, trace] {
                TraceScope context(trace);
                command.resume();
            });
        }))
        throw std::logic_error("account journal already has a pending write");
}

AccountJournal::Result AccountJournal::Write::await_resume()
{
    if (error)
        std::rethrow_exception(error);
    return std::move(result);
}

AccountJournal::Write AccountJournal::append(JournalEntry entry)
{
    return {
        *this,
        [entry = std::move(entry)](SqliteJournal& journal) {
            journal.append(entry.body, entry.command_id, entry.order_id, entry.excluded_order);
            Result result;
            result.capacity = journal.capacity();
            return result;
        },
        {},
        {}
    };
}

AccountJournal::Write AccountJournal::find_command(std::string id)
{
    return {
        *this,
        [id = std::move(id)](SqliteJournal& journal) {
            Result result;
            if (const auto record = journal.command(id); !record.is_null())
                result.command = std::get<CommandRecord>(parse_record(record));
            return result;
        },
        {},
        {}
    };
}

AccountJournal::Write AccountJournal::find_order(std::string id)
{
    return {
        *this,
        [id = std::move(id)](SqliteJournal& journal) {
            Result result;
            result.order_known = !journal.order(id).is_null();
            return result;
        },
        {},
        {}
    };
}

AccountJournal::Write AccountJournal::restore_orders(std::string day, std::uint64_t after)
{
    return {
        *this,
        [day = std::move(day), after](SqliteJournal& journal) {
            Result result;
            for (const auto& index : journal.order_identities(day, after)) {
                result.orders[{day, index.broker_key}] = {index.order_id, index.sequence};
                result.order_cursor = index.sequence;
            }
            return result;
        },
        {},
        {}
    };
}

} // namespace asterion::trading
