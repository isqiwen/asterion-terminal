#include "strategy_store.hpp"
#include "task_store.hpp"
#include "logic_store.hpp"
#include <algorithm>
#include <asterion/protocol/research_logic.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/resource.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/process/file_lock.hpp>

namespace asterion::research {
namespace {

using Database = sqlite::Database;
using Statement = Database::Statement;

void owner(const resource::proto::Owner& value)
{
    protocol::validate_message(value);
    validate_id(value.node_id());
    validate_id(value.service_id());
    if (value.domain() != "research")
        throw std::invalid_argument("research strategy owner required");
}

bool inspect(Database& db, const resource::proto::Owner& expected)
{
    Statement tasks(db, "SELECT id FROM tasks LIMIT 1");
    (void)tasks.step();
    Statement tables(
        db,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('research_strategy_owner','research_strategy_packages','research_strategy_deliveries','research_strategy_commands')");
    (void)tables.step();
    if (!tables.integer(0))
        return false;
    if (tables.integer(0) != 4)
        throw std::invalid_argument("incomplete research strategy schema");
    Statement read(
        db,
        "SELECT CASE WHEN length(body)<=4096 THEN body END FROM research_strategy_owner WHERE singleton=1");
    resource::proto::Owner actual;
    if (!read.step() || !actual.ParseFromString(read.blob(0)))
        throw std::invalid_argument("invalid research strategy owner");
    owner(actual);
    if (actual.SerializeAsString() != expected.SerializeAsString())
        throw Error(ErrorCode::conflict, "research strategy owner mismatch");
    return true;
}

void request(const proto::StrategyMutation& value)
{
    protocol::validate_message(value);
    validate_id(value.id());
    if (value.ByteSizeLong() > 65536)
        throw std::invalid_argument("invalid research strategy mutation");
    if (value.has_freeze()) {
        validate_id(value.freeze().id());
        protocol::validate_strategy_object(value.freeze().source(), "backtest_result");
    } else if (value.has_logic()) {
        validate_id(value.logic().id());
        protocol::validate_strategy_object(value.logic().source(), "logic_trial_result");
    } else if (value.has_retire())
        protocol::validate_strategy_object(value.retire().package(), "strategy_package");
    else if (value.has_deliver()) {
        validate_id(value.deliver().id());
        validate_id(value.deliver().target_task_id());
        protocol::validate_strategy_object(value.deliver().package(), "strategy_package");
    } else
        throw std::invalid_argument("invalid research strategy mutation");
}

void receipt_shape(
    const proto::StrategyMutation& req,
    const proto::StrategyReply& reply,
    const resource::proto::Owner& expected)
{
    request(req);
    protocol::validate_message(reply);
    const bool pkg = req.has_freeze() || req.has_logic() || req.has_retire();
    if (reply.has_package() != pkg || reply.has_delivery() != req.has_deliver()
        || reply.packages_size() || reply.deliveries_size() || !reply.next_after().empty()
        || reply.target_accepted() || reply.ByteSizeLong() > 65536)
        throw std::invalid_argument("invalid research strategy receipt");
    if (pkg) {
        const auto& value = reply.package();
        protocol::validate_strategy_package(value);
        if (value.owner().SerializeAsString() != expected.SerializeAsString())
            throw Error(ErrorCode::conflict, "research strategy owner mismatch");
        if (req.has_freeze()) {
            const auto& r = req.freeze();
            if (value.id() != r.id() || value.name() != r.name() || value.purpose() != r.purpose()
                || value.limitations() != r.limitations()
                || value.reference_result().SerializeAsString() != r.source().SerializeAsString()
                || !value.retirement_reason().empty())
                throw std::invalid_argument("invalid research strategy receipt");
        } else if (req.has_logic()) {
            const auto& r = req.logic();
            if (!value.has_logic() || value.id() != r.id() || value.name() != r.name()
                || value.purpose() != r.purpose() || value.limitations() != r.limitations()
                || value.logic().result().SerializeAsString() != r.source().SerializeAsString()
                || value.definition().contract().SerializeAsString()
                    != r.contract().SerializeAsString()
                || value.definition().strategy().SerializeAsString()
                    != r.strategy().SerializeAsString()
                || value.definition().interval_minutes() != r.interval_minutes()
                || !value.retirement_reason().empty())
                throw std::invalid_argument("invalid research strategy receipt");
        } else if (protocol::strategy_package_object(value).SerializeAsString()
                != req.retire().package().SerializeAsString()
            || value.retirement_reason() != req.retire().reason()
            || value.retirement_reason().empty())
            throw std::invalid_argument("invalid research strategy receipt");
    } else {
        const auto& value = reply.delivery();
        protocol::validate_strategy_delivery(value);
        const auto& r = req.deliver();
        if (value.owner().SerializeAsString() != expected.SerializeAsString()
            || value.id() != r.id() || value.target_task_id() != r.target_task_id()
            || value.package().SerializeAsString() != r.package().SerializeAsString())
            throw std::invalid_argument("invalid research strategy receipt");
    }
}

std::optional<task::proto::StoredTaskRecord> task_record(Database& db, const std::string& id)
{
    validate_id(id);
    Statement read(
        db,
        "SELECT CASE WHEN length(record)<=65536 THEN record END FROM tasks WHERE id=?");
    read.bind(1, id);
    if (!read.step())
        return std::nullopt;
    task::proto::StoredTaskRecord record;
    if (!record.ParseFromString(read.blob(0)) || record.task().id() != id)
        throw std::invalid_argument("invalid research strategy task evidence");
    protocol::validate_message(record);
    return record;
}

task::proto::StoredTaskRecord source(Database& db, const proto::StrategyPackage& value, bool active)
{
    const auto& identity = protocol::strategy_source(value);
    const auto record = task_record(db, identity.id());
    if (!record
        || record->task().kind()
            != (value.has_logic() ? task::proto::LOGIC_TRIAL : task::proto::BACKTEST)
        || record->task().state() != task::proto::SUCCEEDED
        || record->task().result_digest() != identity.version()
        || record->input_digest() != value.source_input_sha256()
        || record->task().attempt() != value.source_attempt()
        || record->task().program_artifact() != value.definition().program_artifact()
        || (!value.has_logic()
            && record->task().risk_artifact() != value.definition().risk_artifact())
        || (active && record->task().has_retirement()))
        throw Error(ErrorCode::conflict, "research strategy source evidence mismatch");
    return *record;
}

void page(const std::string& after, unsigned limit)
{
    if (!after.empty())
        validate_id(after);
    if (!limit || limit > 200)
        throw std::invalid_argument("research strategy page limit must be 1..200");
}

} // namespace

proto::StrategyPackage capture_strategy_package(
    const resource::proto::Owner& expected,
    const proto::StrategyFreeze& req,
    const task::proto::TaskResponse& verified,
    const std::string& input_digest)
{
    protocol::validate_message(req);
    protocol::validate_strategy_object(req.source(), "backtest_result");
    const auto& task = verified.result_task();
    if (req.source().owner().SerializeAsString() != expected.SerializeAsString()
        || !verified.has_result_task() || !verified.has_backtest()
        || task.kind() != task::proto::BACKTEST || task.state() != task::proto::SUCCEEDED
        || task.has_retirement() || !task.has_input() || task.id() != req.source().id()
        || task.result_digest() != req.source().version())
        throw Error(ErrorCode::conflict, "research strategy source evidence mismatch");
    proto::StrategyPackage value;
    value.set_id(req.id());
    *value.mutable_owner() = expected;
    value.set_name(req.name());
    value.set_purpose(req.purpose());
    value.set_limitations(req.limitations());
    *value.mutable_definition() =
        protocol::strategy_definition(task.input(), task.program_artifact(), task.risk_artifact());
    value.set_digest(protocol::strategy_digest(value.definition()));
    *value.mutable_reference_result() = req.source();
    value.set_source_input_sha256(input_digest);
    value.set_source_attempt(task.attempt());
    protocol::validate_strategy_package(value);
    return value;
}

proto::StrategyPackage capture_logic_strategy_package(
    const resource::proto::Owner& expected,
    const proto::StrategyLogicFreeze& req,
    const task::proto::TaskResponse& verified,
    const std::string& input_digest,
    const std::string& risk_artifact)
{
    protocol::validate_message(req);
    protocol::validate_strategy_object(req.source(), "logic_trial_result");
    const auto& task = verified.result_task();
    if (req.source().owner().SerializeAsString() != expected.SerializeAsString()
        || task.kind() != task::proto::LOGIC_TRIAL || task.state() != task::proto::SUCCEEDED
        || task.has_retirement() || !task.has_logic_trial() || !verified.has_logic_trial()
        || task.id() != req.source().id() || task.result_digest() != req.source().version()
        || sha256_bytes(task.logic_trial().SerializeAsString()) != input_digest
        || task.logic_trial().owner().SerializeAsString() != expected.SerializeAsString())
        throw Error(ErrorCode::conflict, "research strategy source evidence mismatch");
    protocol::validate_logic_trial_result(task.logic_trial(), verified.logic_trial());
    if (!verified.logic_trial().all_match() || !verified.logic_trial().decisions())
        throw Error(
            ErrorCode::conflict,
            "strategy requires matching prior expectations and a tested direction");
    proto::StrategyPackage value;
    value.set_id(req.id());
    *value.mutable_owner() = expected;
    value.set_name(req.name());
    value.set_purpose(req.purpose());
    value.set_limitations(req.limitations());
    auto& definition = *value.mutable_definition();
    *definition.mutable_strategy() = req.strategy();
    *definition.mutable_contract() = req.contract();
    definition.set_interval_minutes(req.interval_minutes());
    definition.set_program_artifact(task.program_artifact());
    definition.set_risk_artifact(risk_artifact);
    *value.mutable_logic()->mutable_definition() = task.logic_trial().definition();
    *value.mutable_logic()->mutable_example() =
        protocol::logic_object(task.logic_trial().example());
    *value.mutable_logic()->mutable_result() = req.source();
    value.set_source_input_sha256(input_digest);
    value.set_source_attempt(task.attempt());
    value.set_digest(protocol::strategy_digest(definition));
    protocol::validate_strategy_package(value);
    return value;
}

void Strategies::verify_source(const proto::StrategyPackage& value, bool admission) const
{
    (void)source(*database_, value, admission);
    if (value.has_logic()) {
        Logic view(*database_, owner_, Database::Access::read_only, clock_);
        const auto current = view.input(value.logic().example(), admission);
        auto definition = current.definition();
        definition.clear_retirement_reason();
        auto original = value.logic().definition();
        original.clear_retirement_reason();
        if (definition.SerializeAsString() != original.SerializeAsString())
            throw Error(ErrorCode::conflict, "strategy logic evidence identity mismatch");
    }
}

Strategies::Strategies(
    tasks::Store& store,
    resource::proto::Owner expected,
    Database::Access access,
    std::shared_ptr<const Clock> clock)
    : Strategies(
          store.database(expected.service_id(), access == Database::Access::writer),
          expected,
          access,
          std::move(clock))
{}

Strategies::Strategies(
    Database& db,
    resource::proto::Owner expected,
    Database::Access access,
    std::shared_ptr<const Clock> clock)
    : database_(&db),
      owner_(std::move(expected)),
      writer_(access == Database::Access::writer),
      clock_(std::move(clock))
{
    owner(owner_);
    if (writer_ && !inspect(db, owner_)) {
        Database::Transaction tx(db);
        db.execute(
            "CREATE TABLE research_strategy_owner(singleton INTEGER PRIMARY KEY CHECK(singleton=1),body BLOB NOT NULL) STRICT");
        db.execute(
            "CREATE TABLE research_strategy_packages(id TEXT PRIMARY KEY,source_id TEXT NOT NULL,retired INTEGER NOT NULL CHECK(retired IN (0,1)),body BLOB NOT NULL) STRICT");
        db.execute(
            "CREATE INDEX research_strategy_sources ON research_strategy_packages(source_id,retired)");
        db.execute(
            "CREATE TABLE research_strategy_deliveries(id TEXT PRIMARY KEY,package_id TEXT NOT NULL,target_task_id TEXT NOT NULL UNIQUE,body BLOB NOT NULL) STRICT");
        db.execute(
            "CREATE TABLE research_strategy_commands(id TEXT PRIMARY KEY,request BLOB NOT NULL,response BLOB NOT NULL) STRICT");
        Statement write(db, "INSERT INTO research_strategy_owner VALUES(1,?)");
        write.bind_blob(1, owner_.SerializeAsString()).step();
        tx.commit();
    } else if (!writer_)
        (void)inspect(db, owner_);
}

std::optional<proto::StrategyReply> Strategies::receipt(const proto::StrategyMutation& req) const
{
    request(req);
    if (!inspect(*database_, owner_))
        return std::nullopt;
    Statement read(
        *database_,
        "SELECT CASE WHEN length(request)<=65536 THEN request END,CASE WHEN length(response)<=65536 THEN response END FROM research_strategy_commands WHERE id=?");
    read.bind(1, req.id());
    if (!read.step())
        return std::nullopt;
    if (read.blob(0) != req.SerializeAsString())
        throw Error(ErrorCode::conflict, "strategy request id reused with different content");
    proto::StrategyReply result;
    if (!result.ParseFromString(read.blob(1)))
        throw std::invalid_argument("invalid research strategy receipt");
    receipt_shape(req, result, owner_);
    return result;
}

proto::StrategyPackage Strategies::package(const std::string& id) const
{
    validate_id(id);
    if (!inspect(*database_, owner_))
        throw Error(ErrorCode::not_found, "unknown research strategy package");
    Statement read(
        *database_,
        "SELECT source_id,retired,CASE WHEN length(body)<=32768 THEN body END FROM research_strategy_packages WHERE id=?");
    read.bind(1, id);
    proto::StrategyPackage value;
    if (!read.step())
        throw Error(ErrorCode::not_found, "unknown research strategy package");
    if (!value.ParseFromString(read.blob(2)) || value.id() != id
        || protocol::strategy_source(value).id() != read.text(0)
        || (value.retirement_reason().empty() ? 0 : 1) != read.integer(1))
        throw std::invalid_argument("invalid research strategy package");
    protocol::validate_strategy_package(value);
    if (value.owner().SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "research strategy owner mismatch");
    verify_source(value, false);
    return value;
}

proto::StrategyDelivery Strategies::delivery(const std::string& id) const
{
    validate_id(id);
    if (!inspect(*database_, owner_))
        throw Error(ErrorCode::not_found, "unknown research strategy delivery");
    Statement read(
        *database_,
        "SELECT package_id,target_task_id,CASE WHEN length(body)<=8192 THEN body END FROM research_strategy_deliveries WHERE id=?");
    read.bind(1, id);
    proto::StrategyDelivery value;
    if (!read.step())
        throw Error(ErrorCode::not_found, "unknown research strategy delivery");
    if (!value.ParseFromString(read.blob(2)) || value.id() != id
        || value.package().id() != read.text(0) || value.target_task_id() != read.text(1))
        throw std::invalid_argument("invalid research strategy delivery");
    protocol::validate_strategy_delivery(value);
    const auto fixed = package(value.package().id());
    if (value.owner().SerializeAsString() != owner_.SerializeAsString()
        || value.package().SerializeAsString()
            != protocol::strategy_package_object(fixed).SerializeAsString()
        || protocol::strategy_source(value).SerializeAsString()
            != protocol::strategy_source(fixed).SerializeAsString()
        || value.source_input_sha256() != fixed.source_input_sha256())
        throw Error(ErrorCode::conflict, "research strategy delivery evidence mismatch");
    return value;
}

std::vector<proto::StrategyPackage>
Strategies::packages(const std::string& after, unsigned limit) const
{
    page(after, limit);
    std::vector<proto::StrategyPackage> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement read(
        *database_,
        "SELECT id FROM research_strategy_packages WHERE id>? ORDER BY id LIMIT ?");
    read.bind(1, after).bind(2, static_cast<std::int64_t>(limit));
    while (read.step())
        result.push_back(package(read.text(0)));
    return result;
}

std::vector<proto::StrategyDelivery>
Strategies::deliveries(const std::string& id, const std::string& after, unsigned limit) const
{
    page(after, limit);
    if (!id.empty())
        validate_id(id);
    std::vector<proto::StrategyDelivery> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement read(
        *database_,
        id.empty()
            ? "SELECT id FROM research_strategy_deliveries WHERE id>?1 ORDER BY id LIMIT ?2"
            : "SELECT id FROM research_strategy_deliveries WHERE id>?1 AND package_id=?3 ORDER BY id LIMIT ?2");
    read.bind(1, after).bind(2, static_cast<std::int64_t>(limit));
    if (!id.empty())
        read.bind(3, id);
    while (read.step())
        result.push_back(delivery(read.text(0)));
    return result;
}

proto::StrategyReply
Strategies::apply(const proto::StrategyMutation& req, const proto::StrategyPackage* captured)
{
    if (!writer_)
        throw Error(ErrorCode::permission_denied, "research strategy access is read only");
    request(req);
    Database::Transaction tx(*database_);
    if (const auto replay = receipt(req))
        return *replay;
    proto::StrategyReply reply;
    if (req.has_freeze() || req.has_logic()) {
        if (!captured)
            throw std::invalid_argument("strategy candidate source verification required");
        auto value = *captured;
        value.set_recorded_at_ms(clock_->utc_now() / 1000000);
        *reply.mutable_package() = value;
        receipt_shape(req, reply, owner_);
        verify_source(value, true);
        Statement exists(*database_, "SELECT 1 FROM research_strategy_packages WHERE id=?");
        exists.bind(1, value.id());
        if (exists.step())
            throw Error(ErrorCode::conflict, "research strategy package already exists");
        Statement write(*database_, "INSERT INTO research_strategy_packages VALUES(?,?,0,?)");
        write.bind(1, value.id())
            .bind(2, protocol::strategy_source(value).id())
            .bind_blob(3, value.SerializeAsString())
            .step();
    } else if (req.has_retire()) {
        auto value = package(req.retire().package().id());
        if (protocol::strategy_package_object(value).SerializeAsString()
            != req.retire().package().SerializeAsString())
            throw Error(ErrorCode::conflict, "research strategy package identity changed");
        if (!value.retirement_reason().empty()
            && value.retirement_reason() != req.retire().reason())
            throw Error(ErrorCode::conflict, "strategy deprecation reason is already fixed");
        value.set_retirement_reason(req.retire().reason());
        *reply.mutable_package() = value;
        receipt_shape(req, reply, owner_);
        Statement write(
            *database_,
            "UPDATE research_strategy_packages SET retired=1,body=? WHERE id=?");
        write.bind_blob(1, value.SerializeAsString()).bind(2, value.id()).step();
    } else {
        const auto fixed = package(req.deliver().package().id());
        if (protocol::strategy_package_object(fixed).SerializeAsString()
            != req.deliver().package().SerializeAsString())
            throw Error(ErrorCode::conflict, "research strategy package identity changed");
        if (!fixed.retirement_reason().empty())
            throw Error(ErrorCode::conflict, "research strategy package is deprecated");
        verify_source(fixed, true);
        Statement existing(
            *database_,
            "SELECT 1 FROM tasks WHERE id=?1 UNION ALL SELECT 1 FROM research_strategy_deliveries WHERE target_task_id=?1");
        existing.bind(1, req.deliver().target_task_id());
        if (existing.step())
            throw Error(ErrorCode::conflict, "strategy delivery task identity is already used");
        auto& value = *reply.mutable_delivery();
        value.set_id(req.deliver().id());
        *value.mutable_owner() = owner_;
        *value.mutable_package() = req.deliver().package();
        value.set_target_task_id(req.deliver().target_task_id());
        if (fixed.has_logic())
            *value.mutable_logic_trial() = fixed.logic().result();
        else
            *value.mutable_reference_result() = fixed.reference_result();
        value.set_source_input_sha256(fixed.source_input_sha256());
        value.set_recorded_at_ms(clock_->utc_now() / 1000000);
        value.set_digest(protocol::strategy_delivery_digest(value));
        receipt_shape(req, reply, owner_);
        Statement exists(*database_, "SELECT 1 FROM research_strategy_deliveries WHERE id=?");
        exists.bind(1, value.id());
        if (exists.step())
            throw Error(ErrorCode::conflict, "research strategy delivery already exists");
        Statement write(*database_, "INSERT INTO research_strategy_deliveries VALUES(?,?,?,?)");
        write.bind(1, value.id())
            .bind(2, value.package().id())
            .bind(3, value.target_task_id())
            .bind_blob(4, value.SerializeAsString())
            .step();
    }
    receipt_shape(req, reply, owner_);
    Statement write(*database_, "INSERT INTO research_strategy_commands VALUES(?,?,?)");
    write.bind(1, req.id())
        .bind_blob(2, req.SerializeAsString())
        .bind_blob(3, reply.SerializeAsString())
        .step();
    tx.commit();
    return reply;
}

std::vector<std::string> Strategies::source_consumers(const std::string& id) const
{
    validate_id(id);
    std::vector<std::string> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement read(
        *database_,
        "SELECT id FROM research_strategy_packages WHERE source_id=? AND retired=0 ORDER BY id");
    read.bind(1, id);
    while (read.step()) {
        const auto fixed = package(read.text(0));
        if (!fixed.retirement_reason().empty())
            throw std::invalid_argument("invalid research strategy package");
        result.push_back("strategy_package/" + fixed.id());
    }
    // A deprecated package releases its own hold. Actual admitted consumers
    // retain the logic trial until those tasks are explicitly retired.
    Statement packages(
        *database_,
        "SELECT id FROM research_strategy_packages WHERE source_id=? ORDER BY id");
    packages.bind(1, id);
    while (packages.step()) {
        const auto fixed = package(packages.text(0));
        if (!fixed.has_logic())
            continue;
        std::string cursor;
        for (;;) {
            const auto rows = deliveries(fixed.id(), cursor, 200);
            if (rows.empty())
                break;
            for (const auto& draft : rows) {
                const auto consumer = task_record(*database_, draft.target_task_id());
                if (consumer && !consumer->task().has_retirement()
                    && delivery_status(draft.id()).target_accepted())
                    result.push_back(draft.target_task_id());
            }
            cursor = rows.back().id();
        }
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void Strategies::verify_use(
    const proto::StrategyUse& use,
    const std::string& target,
    bool admission) const
{
    protocol::validate_strategy_use(use);
    const auto fixed = package(use.package().id());
    const auto draft = delivery(use.delivery().id());
    const bool explicit_request = !use.request_sha256().empty();
    const bool accepts_explicit = fixed.has_logic() || fixed.definition().has_portfolio();
    if (protocol::strategy_package_object(fixed).SerializeAsString()
            != use.package().SerializeAsString()
        || protocol::strategy_delivery_object(draft).SerializeAsString()
            != use.delivery().SerializeAsString()
        || draft.package().SerializeAsString() != use.package().SerializeAsString()
        || draft.target_task_id() != target || (fixed.has_logic() && !explicit_request)
        || (explicit_request && !accepts_explicit))
        throw Error(ErrorCode::conflict, "research strategy use evidence mismatch");
    if (admission) {
        if (!fixed.retirement_reason().empty())
            throw Error(ErrorCode::conflict, "research strategy package is deprecated");
        verify_source(fixed, true);
    }
}

proto::StrategyReply Strategies::delivery_status(const std::string& id) const
{
    proto::StrategyReply reply;
    *reply.mutable_delivery() = delivery(id);
    const auto& draft = reply.delivery();
    const auto fixed = package(draft.package().id());
    const auto task = task_record(*database_, draft.target_task_id());
    if (!task)
        return reply;
    proto::StrategyUse expected;
    *expected.mutable_package() = draft.package();
    *expected.mutable_delivery() = protocol::strategy_delivery_object(draft);
    const bool explicit_request = !task->task().strategy_use().request_sha256().empty();
    if (explicit_request)
        expected.set_request_sha256(task->task().strategy_use().request_sha256());
    verify_use(expected, draft.target_task_id(), false);
    const auto& value = task->task();
    if (value.kind() != task::proto::BACKTEST
        || value.strategy_use().SerializeAsString() != expected.SerializeAsString()
        || (!explicit_request && task->input_digest() != draft.source_input_sha256())
        || value.program_artifact() != fixed.definition().program_artifact()
        || value.risk_artifact() != fixed.definition().risk_artifact()
        || (!explicit_request
            && (value.metadata().derivation().source_id() != draft.reference_result().id()
                || value.metadata().derivation().source_result_digest()
                    != draft.reference_result().version())))
        throw Error(ErrorCode::conflict, "strategy delivery target names a different task");
    reply.set_target_accepted(true);
    return reply;
}

resource::proto::Catalogue Strategies::resources(const resource::proto::CatalogueQuery& query) const
{
    const auto limit = protocol::catalogue_limit(query);
    resource::proto::Catalogue result;
    *result.mutable_owner() = owner_;
    result.set_kind(query.kind());
    if (query.kind() == "strategy_package") {
        const auto rows = packages(query.after(), limit);
        for (const auto& value : rows) {
            auto* item = result.add_items();
            *item->mutable_object() = protocol::strategy_package_object(value);
            item->set_name(value.name());
            item->set_state(
                value.retirement_reason().empty() ? resource::proto::CatalogueEntry::SEALED
                                                  : resource::proto::CatalogueEntry::RETIRED);
            item->set_relative_location("tasks.sqlite");
            item->set_logical_bytes(value.ByteSizeLong());
        }
        if (rows.size() == limit && !packages(rows.back().id(), 1).empty())
            result.set_next_after(rows.back().id());
    } else if (query.kind() == "strategy_delivery") {
        const auto rows = deliveries("", query.after(), limit);
        for (const auto& value : rows) {
            auto* item = result.add_items();
            *item->mutable_object() = protocol::strategy_delivery_object(value);
            item->set_name(value.target_task_id());
            item->set_state(resource::proto::CatalogueEntry::SEALED);
            item->set_relative_location("tasks.sqlite");
            item->set_logical_bytes(value.ByteSizeLong());
        }
        if (rows.size() == limit && !deliveries("", rows.back().id(), 1).empty())
            result.set_next_after(rows.back().id());
    } else
        throw Error(ErrorCode::invalid_request, "resource kind is not implemented by Research");
    return result;
}

void Strategies::inspect_backup(
    const std::filesystem::path& root,
    const resource::proto::Owner& expected,
    std::span<const std::pair<std::string, proto::StrategyUse>> required,
    const std::function<void(const proto::StrategyPackage&)>& packages_visitor,
    const std::function<void(const proto::StrategyDelivery&)>& deliveries_visitor,
    std::stop_token stop)
{
    owner(expected);
    FileLock cut(root, "manager.lock", FileLock::Access::shared_existing);
    Database db(root / "tasks.sqlite", Database::Access::cold_read_only);
    Strategies view(
        db,
        expected,
        Database::Access::cold_read_only,
        std::make_shared<SystemClock>());
    const auto check = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "Research strategy cold inspection interrupted");
    };
    for (const auto& [id, use] : required) {
        check();
        view.verify_use(use, id, false);
    }
    std::string cursor;
    for (;;) {
        check();
        const auto rows = view.packages(cursor, 200);
        if (rows.empty())
            break;
        for (const auto& value : rows) {
            check();
            packages_visitor(value);
        }
        cursor = rows.back().id();
    }
    cursor.clear();
    for (;;) {
        check();
        const auto rows = view.deliveries("", cursor, 200);
        if (rows.empty())
            break;
        for (const auto& value : rows) {
            check();
            const auto status = view.delivery_status(value.id());
            (void)status;
            deliveries_visitor(value);
        }
        cursor = rows.back().id();
    }
    if (!inspect(db, expected))
        return;
    Statement receipts(
        db,
        "SELECT id,CASE WHEN length(request)<=65536 THEN request END,CASE WHEN length(response)<=65536 THEN response END FROM research_strategy_commands ORDER BY id");
    while (receipts.step()) {
        check();
        proto::StrategyMutation req;
        proto::StrategyReply reply;
        if (!req.ParseFromString(receipts.blob(1)) || !reply.ParseFromString(receipts.blob(2))
            || req.id() != receipts.text(0))
            throw std::invalid_argument("invalid research strategy receipt");
        receipt_shape(req, reply, expected);
        if (reply.has_package()) {
            auto current = view.package(reply.package().id());
            auto recorded = reply.package();
            if (req.has_freeze()) {
                current.clear_retirement_reason();
                recorded.clear_retirement_reason();
            }
            if (current.SerializeAsString() != recorded.SerializeAsString())
                throw Error(
                    ErrorCode::conflict,
                    "strategy receipt differs from its immutable package");
        } else if (view.delivery(reply.delivery().id()).SerializeAsString()
            != reply.delivery().SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "strategy receipt differs from its immutable delivery");
    }
}

} // namespace asterion::research
