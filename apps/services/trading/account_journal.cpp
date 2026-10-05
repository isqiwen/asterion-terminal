#include "account_journal.hpp"
#include <asterion/kernel/trace.hpp>
#include <asterion/foundation/error.hpp>
namespace asterion::trading {
namespace {
std::string excluded_order(const Json& record) {
  if (record.contains("order_not_sent"))
    return record.at("order_not_sent").get<std::string>();
  if (record.contains("command") && record.at("command").at("action") == "live_resolve")
    return record.at("command").at("order_id").get<std::string>();
  return {};
}
} // namespace
AccountJournal::AccountJournal(std::filesystem::path directory,
                               std::function<void(const Json&)> validate, Post post,
                               Progress& progress)
    : progress_(progress), post_(std::move(post)) {
  std::promise<void> started;
  auto ready = started.get_future();
  thread_ = std::jthread([this, directory = std::move(directory), validate = std::move(validate),
                          started = std::move(started)]() mutable {
    progress_.begin();
    std::unique_ptr<SqliteJournal> journal;
    try {
      journal = std::make_unique<SqliteJournal>(
          directory, std::set<std::string>{"plugins", "ctp-flow", "archives"}, std::move(validate));
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
AccountJournal::~AccountJournal() {
  jobs_.close();
  thread_.join();
}
void AccountJournal::replay(const std::function<void(std::uint64_t, const Json&)>& apply) {
  std::uint64_t submitted = 0;
  for (std::uint64_t first = 0; first < initial_capacity_.total_records;) {
    auto task = std::make_shared<std::packaged_task<std::vector<Json>(SqliteJournal&)>>(
        [first, &submitted, total = initial_capacity_.total_records](SqliteJournal& journal) {
          auto page = journal.read(first);
          auto sequence = first;
          for (const auto& record : page) {
            if (record.contains("command") &&
                journal.command_sequence(
                    record.at("command").at("request_id").get<std::string>()) != sequence)
              throw std::invalid_argument(
                  "trading command index does not match its journal record");
            if (record.contains("command") && record.at("command").at("action") == "submit") {
              ++submitted;
              const auto id = record.at("command").at("order_id").get<std::string>();
              const auto index = journal.order_index(id);
              if (!index || index->sequence != sequence ||
                  index->trading_day != record.at("trading_day").get<std::string>() ||
                  index->broker_key != record.at("broker_key").get<std::string>())
                throw std::invalid_argument(
                    "trading order index does not match its journal record");
              if (index->excluded_by) {
                const auto exclusion = journal.record(index->excluded_by);
                if (excluded_order(exclusion) != id)
                  throw std::invalid_argument(
                      "trading order index does not match its journal record");
              }
            }
            const auto excluded = excluded_order(record);
            if (!excluded.empty()) {
              const auto index = journal.order_index(excluded);
              if (!index || index->excluded_by != sequence)
                throw std::invalid_argument(
                    "trading order index does not match its journal record");
            }
            ++sequence;
          }
          // Unique identities plus per-record checks and equal cardinality rule out
          // an extra index row that could otherwise claim an unrelated broker report.
          if (sequence == total && journal.order_identity_count() != submitted)
            throw std::invalid_argument("trading order index does not match its journal record");
          return page;
        });
    auto result = task->get_future();
    if (!jobs_.try_push([task](SqliteJournal& journal) { (*task)(journal); }))
      throw std::logic_error("account journal already has a pending write");
    const auto page = result.get();
    for (const auto& record : page)
      apply(first++, record);
  }
}
SqliteJournal::Capacity AccountJournal::initialize(Json header) {
  auto task = std::make_shared<std::packaged_task<SqliteJournal::Capacity(SqliteJournal&)>>(
      [header = std::move(header)](SqliteJournal& journal) {
        journal.append(header);
        return journal.capacity();
      });
  auto result = task->get_future();
  if (!jobs_.try_push([task](SqliteJournal& journal) { (*task)(journal); }))
    throw std::logic_error("account journal already has a pending write");
  return result.get();
}
void AccountJournal::Write::await_suspend(std::coroutine_handle<> command) {
  const auto trace = std::string(current_trace_id());
  if (!owner.jobs_.try_push([this, command, trace](SqliteJournal& journal) {
        TraceScope context(trace);
        try {
          result = operation(journal);
        } catch (...) {
          error = std::current_exception();
        }
        retiring.reset();
        // The suspended command owns this awaiter. Posting publishes its result;
        // the writer must not touch it after the account can resume.
        owner.post_([command, trace] {
          TraceScope context(trace);
          command.resume();
        });
      }))
    throw std::logic_error("account journal already has a pending write");
}
AccountJournal::Result AccountJournal::Write::await_resume() {
  if (error)
    std::rethrow_exception(error);
  return std::move(result);
}
AccountJournal::Write AccountJournal::append(Json record) {
  return {*this,
          [record = std::move(record)](SqliteJournal& journal) {
            const auto id = record.contains("command")
                                ? record.at("command").at("request_id").get<std::string>()
                                : std::string{};
            const auto order_id =
                record.contains("command") && record.at("command").at("action") == "submit"
                    ? record.at("command").at("order_id").get<std::string>()
                    : std::string{};
            const auto excluded = excluded_order(record);
            journal.append(record, id, order_id, excluded);
            return Result{journal.capacity(), nullptr, {}};
          },
          {},
          {},
          {}};
}
AccountJournal::Write AccountJournal::find_command(std::string id) {
  return {
      *this,
      [id = std::move(id)](SqliteJournal& journal) { return Result{{}, journal.command(id), {}}; },
      {},
      {},
      {}};
}
AccountJournal::Write AccountJournal::find_order(std::string id) {
  return {
      *this,
      [id = std::move(id)](SqliteJournal& journal) { return Result{{}, journal.order(id), {}}; },
      {},
      {},
      {}};
}
AccountJournal::Write AccountJournal::restore_orders(std::string day, std::uint64_t after) {
  return {*this,
          [day = std::move(day), after](SqliteJournal& journal) {
            Result result;
            for (const auto& index : journal.order_identities(day, after)) {
              result.orders[{day, index.broker_key}] = {index.order_id, index.sequence};
              result.order_cursor = index.sequence;
            }
            return result;
          },
          {},
          {},
          {}};
}
AccountJournal::Write AccountJournal::prepare_policy(std::filesystem::path plugins, Json definition,
                                                     std::string revision,
                                                     std::string requested_artifact,
                                                     std::string current_artifact) {
  return {*this,
          [plugins = std::move(plugins), definition = std::move(definition),
           revision = std::move(revision), requested_artifact = std::move(requested_artifact),
           current_artifact = std::move(current_artifact)](SqliteJournal&) {
            auto algorithm =
                requested_artifact == current_artifact
                    ? risk_providers::Module::pinned(plugins / current_artifact, current_artifact)
                    : risk_providers::Module::selected();
            if (algorithm.artifact() != requested_artifact)
              throw Error(ErrorCode::conflict,
                          "requested policy algorithm is not the deployed risk plugin");
            auto policy =
                std::make_unique<AccountPolicy>(definition, revision, std::move(algorithm));
            policy->capture(plugins);
            return Result{{}, nullptr, std::move(policy)};
          },
          {},
          {},
          {}};
}
AccountJournal::Write AccountJournal::retire_policy(std::unique_ptr<const AccountPolicy> policy) {
  return {*this, [](SqliteJournal&) { return Result{}; }, {}, {}, std::move(policy)};
}
} // namespace asterion::trading
