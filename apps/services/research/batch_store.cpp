#include "batch_store.hpp"
#include "task_store.hpp"
#include <asterion/protocol/research_batch.hpp>
#include <asterion/protocol/research_assessment.hpp>
#include <set>
#include <asterion/protocol/trading.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/foundation/error.hpp>
#include <algorithm>
#include <asterion/protocol/resource.hpp>
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
        throw std::invalid_argument("research batch owner required");
}

bool inspect(Database& db, const resource::proto::Owner& expected)
{
    Statement tasks(db, "SELECT id FROM tasks LIMIT 1");
    (void)tasks.step();
    Statement tables(
        db,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('research_batch_owner','research_batch_states','research_batch_members','research_batch_commands')");
    (void)tables.step();
    if (!tables.integer(0))
        return false;
    if (tables.integer(0) != 4)
        throw std::invalid_argument("incomplete research batch schema");
    Statement read(
        db,
        "SELECT CASE WHEN length(body)<=4096 THEN body END FROM research_batch_owner WHERE singleton=1");
    resource::proto::Owner actual;
    if (!read.step() || !actual.ParseFromString(read.blob(0)))
        throw std::invalid_argument("invalid research batch owner");
    owner(actual);
    if (actual.SerializeAsString() != expected.SerializeAsString())
        throw Error(ErrorCode::conflict, "research batch owner mismatch");
    return true;
}

std::optional<task::proto::StoredTaskRecord> record(Database& db, const std::string& id)
{
    Statement read(
        db,
        "SELECT CASE WHEN length(record)<=65536 THEN record END FROM tasks WHERE id=?");
    read.bind(1, id);
    if (!read.step())
        return std::nullopt;
    task::proto::StoredTaskRecord value;
    if (!value.ParseFromString(read.blob(0)) || value.task().id() != id)
        throw std::invalid_argument("invalid research batch task evidence");
    protocol::validate_message(value);
    return value;
}

proto::BacktestBatchMemberReceipt& member(proto::BacktestBatchState& value, const std::string& id)
{
    for (auto& item : *value.mutable_members())
        if (item.task_id() == id)
            return item;
    throw Error(ErrorCode::not_found, "unknown backtest batch member");
}

const proto::BacktestBatchMemberReceipt&
member(const proto::BacktestBatchState& value, const std::string& id)
{
    for (const auto& item : value.members())
        if (item.task_id() == id)
            return item;
    throw Error(ErrorCode::not_found, "unknown backtest batch member");
}

bool terminal(task::proto::TaskState state)
{
    return state == task::proto::SUCCEEDED || state == task::proto::FAILED
        || state == task::proto::CANCELLED || state == task::proto::INTERRUPTED;
}

void same(const proto::BacktestBatchState& value, const resource::proto::Object& object)
{
    if (protocol::backtest_batch_object(value.definition()).SerializeAsString()
        != object.SerializeAsString())
        throw Error(ErrorCode::conflict, "backtest batch identity mismatch");
}

void receipt_shape(
    const proto::BacktestBatchMutation& command,
    const proto::BacktestBatchState& value,
    const resource::proto::Owner& expected)
{
    protocol::validate_backtest_batch_state(value);
    if (value.definition().owner().SerializeAsString() != expected.SerializeAsString())
        throw Error(ErrorCode::conflict, "research batch owner mismatch");
    if (command.has_save()) {
        if (value.definition().request().SerializeAsString() != command.save().SerializeAsString()
            || value.phase() != proto::BATCH_SAVED)
            throw std::invalid_argument("invalid research batch receipt");
    } else
        same(value, command.has_start() ? command.start() : command.stop().batch());
}

void verify_member_record(
    const proto::BacktestBatchState& value,
    const proto::BacktestBatchMemberReceipt& receipt,
    const task::proto::StoredTaskRecord& actual)
{
    const auto& task = actual.task();
    const auto& origin = task.metadata().derivation();
    if (task.kind() != task::proto::BACKTEST || actual.input_digest() != receipt.input_sha256()
        || task.program_artifact() != value.definition().program_artifact()
        || task.risk_artifact() != value.definition().risk_artifact()
        || origin.source_id() != value.definition().request().source().id()
        || origin.source_result_digest() != value.definition().request().source().version()
        || origin.source_instance() != value.definition().owner().service_id()
        || origin.data_choice() != "fixed" || !origin.snapshot().empty()
        || origin.batch().SerializeAsString()
            != protocol::backtest_batch_object(value.definition()).SerializeAsString())
        throw Error(ErrorCode::conflict, "actual task does not match its fixed batch member");
}

bool assessments_present(Database& database)
{
    Statement tables(
        database,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('research_batch_assessments','research_batch_assessment_commands')");
    (void)tables.step();
    if (!tables.integer(0))
        return false;
    if (tables.integer(0) != 2)
        throw std::invalid_argument("incomplete batch assessment schema");
    return true;
}

std::set<std::string> assessment_sources(const proto::BatchAssessment& value)
{
    std::set<std::string> result{value.snapshot().source().id()};
    for (const auto& member : value.snapshot().members())
        result.insert(member.id());
    return result;
}

void assessment_current(Database& database, const proto::BatchAssessmentTask& observed)
{
    const auto actual = record(database, observed.id());
    if (!actual || actual->task().kind() != task::proto::BACKTEST
        || actual->input_digest() != observed.input_sha256()
        || actual->task().state() != observed.state()
        || actual->task().attempt() != observed.attempt()
        || actual->task().has_retirement() != observed.retired()
        || actual->task().error() != observed.error()
        || actual->task().program_artifact() != observed.program_artifact()
        || actual->task().risk_artifact() != observed.risk_artifact()
        || (observed.state() == task::proto::SUCCEEDED
            && actual->task().result_digest() != observed.result_sha256()))
        throw Error(
            ErrorCode::conflict,
            "batch assessment observation changed; read a new preview");
}

} // namespace

Batches::Batches(
    tasks::Store& store,
    resource::proto::Owner expected,
    Database::Access access,
    std::shared_ptr<const Clock> clock)
    : Batches(
          store.research_metadata(expected.service_id(), access == Database::Access::writer),
          expected,
          access,
          std::move(clock))
{}

Batches::Batches(
    Database& database,
    resource::proto::Owner expected,
    Database::Access access,
    std::shared_ptr<const Clock> clock)
    : database_(&database),
      owner_(std::move(expected)),
      writer_(access == Database::Access::writer),
      clock_(std::move(clock))
{
    owner(owner_);
    if (writer_ && !inspect(*database_, owner_)) {
        Database::Transaction tx(*database_);
        database_->execute(
            "CREATE TABLE research_batch_owner(singleton INTEGER PRIMARY KEY CHECK(singleton=1),body BLOB NOT NULL) STRICT");
        database_->execute(
            "CREATE TABLE research_batch_states(id TEXT PRIMARY KEY,source_id TEXT NOT NULL,phase INTEGER NOT NULL,body BLOB NOT NULL) STRICT");
        database_->execute(
            "CREATE INDEX research_batch_source ON research_batch_states(source_id,id)");
        database_->execute(
            "CREATE TABLE research_batch_members(task_id TEXT PRIMARY KEY,batch_id TEXT NOT NULL) STRICT");
        database_->execute(
            "CREATE TABLE research_batch_commands(id TEXT PRIMARY KEY,request BLOB NOT NULL,response BLOB NOT NULL) STRICT");
        Statement write(*database_, "INSERT INTO research_batch_owner VALUES(1,?)");
        write.bind_blob(1, owner_.SerializeAsString()).step();
        tx.commit();
    } else if (!writer_)
        (void)inspect(*database_, owner_);
}

void Batches::writable() const
{
    if (!writer_)
        throw Error(ErrorCode::permission_denied, "research batch access is read only");
}

proto::BacktestBatchState Batches::state(const std::string& id) const
{
    validate_id(id);
    if (!inspect(*database_, owner_))
        throw Error(ErrorCode::not_found, "unknown research batch");
    Statement read(
        *database_,
        "SELECT source_id,phase,CASE WHEN length(body)<=262144 THEN body END FROM research_batch_states WHERE id=?");
    read.bind(1, id);
    if (!read.step())
        throw Error(ErrorCode::not_found, "unknown research batch");
    proto::BacktestBatchState value;
    if (!value.ParseFromString(read.blob(2)))
        throw std::invalid_argument("invalid research batch record");
    protocol::validate_backtest_batch_state(value);
    if (value.definition().request().id() != id
        || value.definition().request().source().id() != read.text(0)
        || value.phase() != read.integer(1)
        || value.definition().owner().SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "research batch index mismatch");
    for (const auto& m : value.members()) {
        Statement index(*database_, "SELECT batch_id FROM research_batch_members WHERE task_id=?");
        index.bind(1, m.task_id());
        if (!index.step() || index.text(0) != id)
            throw Error(ErrorCode::conflict, "research batch member index mismatch");
    }
    return value;
}

std::vector<proto::BacktestBatchState>
Batches::states(const std::string& after, unsigned limit, bool active_only) const
{
    if (!after.empty())
        validate_id(after);
    if (!limit || limit > 200)
        throw std::invalid_argument("research batch page limit must be 1..200");
    std::vector<proto::BacktestBatchState> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement read(
        *database_,
        "SELECT id FROM research_batch_states WHERE id>? AND (?=0 OR phase IN (2,3)) ORDER BY id LIMIT ?");
    read.bind(1, after).bind(2, active_only ? 1 : 0).bind(3, limit);
    while (read.step())
        result.push_back(state(read.text(0)));
    return result;
}

std::optional<proto::BacktestBatchState> Batches::receipt(
    const proto::BacktestBatchMutation& command) const
{
    protocol::validate_backtest_batch_mutation(command);
    if (!inspect(*database_, owner_))
        return std::nullopt;
    Statement read(
        *database_,
        "SELECT CASE WHEN length(request)<=65536 THEN request END,CASE WHEN length(response)<=262144 THEN response END FROM research_batch_commands WHERE id=?");
    read.bind(1, command.id());
    if (!read.step())
        return std::nullopt;
    if (read.blob(0) != command.SerializeAsString())
        throw Error(ErrorCode::conflict, "batch request id reused with different content");
    proto::BacktestBatchState result;
    if (!result.ParseFromString(read.blob(1)))
        throw std::invalid_argument("invalid research batch receipt");
    receipt_shape(command, result, owner_);
    return result;
}

void Batches::verify_source(const proto::BacktestBatchDefinition& definition, bool active) const
{
    const auto source = record(*database_, definition.request().source().id());
    if (!source || source->task().kind() != task::proto::BACKTEST
        || source->task().state() != task::proto::SUCCEEDED
        || source->task().result_digest() != definition.request().source().version()
        || source->input_digest() != definition.source_input_sha256()
        || source->task().program_artifact() != definition.program_artifact()
        || source->task().risk_artifact() != definition.risk_artifact()
        || (active && source->task().has_retirement()))
        throw Error(ErrorCode::conflict, "research batch source evidence mismatch");
}

void Batches::persist(const proto::BacktestBatchState& value)
{
    protocol::validate_backtest_batch_state(value);
    Statement write(*database_, "UPDATE research_batch_states SET phase=?,body=? WHERE id=?");
    write.bind(1, value.phase())
        .bind_blob(2, value.SerializeAsString())
        .bind(3, value.definition().request().id())
        .step();
}

proto::BacktestBatchState Batches::apply(
    const proto::BacktestBatchMutation& command,
    const proto::BacktestBatchDefinition* captured)
{
    writable();
    protocol::validate_backtest_batch_mutation(command);
    Database::Transaction tx(*database_);
    if (const auto previous = receipt(command))
        return *previous;
    proto::BacktestBatchState value;
    if (command.has_save()) {
        if (!captured)
            throw std::invalid_argument("verified original batch definition required");
        protocol::validate_backtest_batch_definition(*captured);
        if (captured->owner().SerializeAsString() != owner_.SerializeAsString()
            || captured->request().SerializeAsString() != command.save().SerializeAsString())
            throw Error(ErrorCode::conflict, "captured batch definition mismatch");
        verify_source(*captured, true);
        Statement exists(*database_, "SELECT 1 FROM research_batch_states WHERE id=?");
        exists.bind(1, command.save().id());
        if (exists.step())
            throw Error(ErrorCode::conflict, "research batch already exists");
        *value.mutable_definition() = *captured;
        value.set_phase(proto::BATCH_SAVED);
        for (const auto& planned : command.save().members()) {
            if (record(*database_, planned.task_id()))
                throw Error(ErrorCode::conflict, "batch target task already exists");
            Statement reserve(*database_, "INSERT INTO research_batch_members VALUES(?,?)");
            reserve.bind(1, planned.task_id()).bind(2, command.save().id()).step();
            auto& m = *value.add_members();
            m.set_task_id(planned.task_id());
            m.set_admission(proto::BATCH_MEMBER_WAITING);
        }
        protocol::validate_backtest_batch_state(value);
        Statement save(*database_, "INSERT INTO research_batch_states VALUES(?,?,?,?)");
        save.bind(1, command.save().id())
            .bind(2, command.save().source().id())
            .bind(3, value.phase())
            .bind_blob(4, value.SerializeAsString())
            .step();
    } else {
        const auto& object = command.has_start() ? command.start() : command.stop().batch();
        value = state(object.id());
        same(value, object);
        if (command.has_start()) {
            if (value.phase() != proto::BATCH_SAVED)
                throw Error(
                    ErrorCode::conflict,
                    "batch cannot start again; create a new batch for unfinished members");
            verify_source(value.definition(), true);
            value.set_phase(proto::BATCH_RUNNING);
            value.set_started_at_ms(clock_->utc_now() / 1000000);
        } else if (value.phase() == proto::BATCH_SAVED || value.phase() == proto::BATCH_RUNNING) {
            value.set_phase(
                value.phase() == proto::BATCH_SAVED ? proto::BATCH_STOPPED : proto::BATCH_STOPPING);
            value.set_reason(command.stop().reason());
            if (value.phase() == proto::BATCH_STOPPED) {
                value.set_stopped_at_ms(clock_->utc_now() / 1000000);
                value.set_elapsed_ms(0);
            }
        }
        persist(value);
    }
    receipt_shape(command, value, owner_);
    Statement write(*database_, "INSERT INTO research_batch_commands VALUES(?,?,?)");
    write.bind(1, command.id())
        .bind_blob(2, command.SerializeAsString())
        .bind_blob(3, value.SerializeAsString())
        .step();
    tx.commit();
    return value;
}

proto::BacktestBatchState Batches::begin_member(
    const std::string& id,
    const std::string& task,
    const std::string& input_digest)
{
    writable();
    Database::Transaction tx(*database_);
    auto value = state(id);
    if (value.phase() != proto::BATCH_RUNNING)
        throw Error(ErrorCode::conflict, "batch is not admitting new members");
    verify_source(value.definition(), true);
    auto& m = member(value, task);
    if (m.admission() != proto::BATCH_MEMBER_WAITING) {
        if (m.input_sha256() != input_digest)
            throw Error(ErrorCode::conflict, "batch member input identity changed");
        return value;
    }
    if (record(*database_, task))
        throw Error(ErrorCode::conflict, "batch target task already exists");
    m.set_admission(proto::BATCH_MEMBER_PENDING);
    m.set_input_sha256(input_digest);
    persist(value);
    tx.commit();
    return value;
}

void Batches::verify_member(const proto::BacktestBatchState& value, const std::string& id) const
{
    const auto actual = record(*database_, id);
    const auto& receipt = member(value, id);
    if (!actual)
        throw Error(ErrorCode::not_found, "batch member admission is not confirmed");
    verify_member_record(value, receipt, *actual);
}

std::vector<task::proto::Task> Batches::member_tasks(const proto::BacktestBatchState& value) const
{
    protocol::validate_backtest_batch_state(value);
    const auto object = protocol::backtest_batch_object(value.definition());
    std::vector<task::proto::Task> result;
    for (const auto& member : value.members()) {
        const auto actual = record(*database_, member.task_id());
        if (!actual) {
            if (member.admission() == proto::BATCH_MEMBER_ADMITTED)
                throw Error(ErrorCode::conflict, "confirmed batch member task is missing");
            continue;
        }
        const bool own_task = actual->task().metadata().derivation().batch().SerializeAsString()
            == object.SerializeAsString();
        if (member.admission() == proto::BATCH_MEMBER_WAITING
            || member.admission() == proto::BATCH_MEMBER_REJECTED) {
            if (own_task)
                throw Error(
                    ErrorCode::conflict,
                    "unconfirmed task occupies a rejected or waiting batch member");
            continue; // An unrelated task ID collision is not a batch task or a cancellation target.
        }
        verify_member(value, member.task_id());
        result.push_back(actual->task());
    }
    return result;
}

void Batches::verify_admission(
    const resource::proto::Object& object,
    const std::string& id,
    const std::string& input_digest) const
{
    const auto value = state(object.id());
    same(value, object);
    const auto& m = member(value, id);
    if (value.phase() != proto::BATCH_RUNNING || m.admission() != proto::BATCH_MEMBER_PENDING
        || m.input_sha256() != input_digest)
        throw Error(ErrorCode::conflict, "batch stopped or member admission identity changed");
    verify_source(value.definition(), true);
}

proto::BacktestBatchState Batches::confirm_member(const std::string& batch, const std::string& id)
{
    writable();
    Database::Transaction tx(*database_);
    auto value = state(batch);
    auto& m = member(value, id);
    if (m.admission() != proto::BATCH_MEMBER_PENDING
        && m.admission() != proto::BATCH_MEMBER_ADMITTED)
        throw Error(ErrorCode::conflict, "batch member was not prepared for admission");
    verify_member(value, id);
    m.set_admission(proto::BATCH_MEMBER_ADMITTED);
    persist(value);
    tx.commit();
    return value;
}

proto::BacktestBatchState Batches::reject_member(
    const std::string& batch,
    const std::string& id,
    const protocol::proto::Error& error)
{
    writable();
    Database::Transaction tx(*database_);
    auto value = state(batch);
    auto& m = member(value, id);
    const auto actual = record(*database_, id);
    const bool own_task = actual
        && actual->task().metadata().derivation().batch().SerializeAsString()
            == protocol::backtest_batch_object(value.definition()).SerializeAsString();
    if ((m.admission() != proto::BATCH_MEMBER_PENDING
            && m.admission() != proto::BATCH_MEMBER_WAITING)
        || own_task)
        throw Error(
            ErrorCode::conflict,
            "batch member rejection cannot replace an actual admission");
    m.set_admission(proto::BATCH_MEMBER_REJECTED);
    *m.mutable_error() = error;
    persist(value);
    tx.commit();
    return value;
}

proto::BacktestBatchState
Batches::finish(const std::string& id, std::optional<std::uint64_t> elapsed_ms)
{
    writable();
    Database::Transaction tx(*database_);
    auto value = state(id);
    if (value.phase() != proto::BATCH_RUNNING && value.phase() != proto::BATCH_STOPPING)
        return value;
    for (const auto& m : value.members()) {
        if (m.admission() == proto::BATCH_MEMBER_PENDING
            || (value.phase() == proto::BATCH_RUNNING
                && m.admission() == proto::BATCH_MEMBER_WAITING))
            return value;
        if (m.admission() == proto::BATCH_MEMBER_ADMITTED) {
            verify_member(value, m.task_id());
            if (!terminal(record(*database_, m.task_id())->task().state()))
                return value;
        }
    }
    value.set_phase(
        value.phase() == proto::BATCH_RUNNING ? proto::BATCH_COMPLETED : proto::BATCH_STOPPED);
    value.set_stopped_at_ms(clock_->utc_now() / 1000000);
    if (elapsed_ms)
        value.set_elapsed_ms(*elapsed_ms);
    else
        value.clear_elapsed_ms();
    persist(value);
    tx.commit();
    return value;
}

std::vector<proto::BacktestBatchState> Batches::interrupt_running()
{
    writable();
    Database::Transaction tx(*database_);
    std::vector<std::string> ids;
    Statement read(
        *database_,
        "SELECT id FROM research_batch_states WHERE phase IN (?,?) ORDER BY id");
    read.bind(1, proto::BATCH_RUNNING).bind(2, proto::BATCH_STOPPING);
    while (read.step())
        ids.push_back(read.text(0));
    std::vector<proto::BacktestBatchState> result;
    for (const auto& id : ids) {
        auto value = state(id);
        for (auto& m : *value.mutable_members())
            if (m.admission() == proto::BATCH_MEMBER_PENDING && record(*database_, m.task_id())) {
                verify_member(value, m.task_id());
                m.set_admission(proto::BATCH_MEMBER_ADMITTED);
            }
        value.set_phase(proto::BATCH_INTERRUPTED);
        value.set_reason("Research restarted; unfinished members require a new batch");
        value.set_stopped_at_ms(clock_->utc_now() / 1000000);
        value.clear_elapsed_ms();
        persist(value);
        result.push_back(std::move(value));
    }
    tx.commit();
    return result;
}

std::vector<std::string> Batches::source_consumers(const std::string& id) const
{
    validate_id(id);
    std::vector<std::string> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement read(
        *database_,
        "SELECT id FROM research_batch_states WHERE source_id=? ORDER BY id");
    read.bind(1, id);
    while (read.step()) {
        const auto value = state(read.text(0));
        if (value.phase() == proto::BATCH_SAVED || value.phase() == proto::BATCH_RUNNING
            || value.phase() == proto::BATCH_STOPPING
            || std::ranges::any_of(value.members(), [](const auto& m) {
                   return m.admission() == proto::BATCH_MEMBER_PENDING;
               }))
            result.push_back(value.definition().request().id());
    }
    return result;
}

proto::BatchAssessment Batches::assessment(const std::string& id) const
{
    validate_id(id);
    if (!assessments_present(*database_))
        throw Error(ErrorCode::not_found, "unknown batch assessment");
    Statement read(
        *database_,
        "SELECT batch_id,CASE WHEN length(body)<=4194304 THEN body END FROM research_batch_assessments WHERE id=?");
    read.bind(1, id);
    if (!read.step())
        throw Error(ErrorCode::not_found, "unknown batch assessment");
    proto::BatchAssessment value;
    if (!value.ParseFromString(read.blob(1)))
        throw std::invalid_argument("invalid batch assessment record");
    protocol::validate_batch_assessment(value);
    if (value.owner().SerializeAsString() != owner_.SerializeAsString()
        || value.declaration().id() != id || value.declaration().batch().id() != read.text(0))
        throw Error(ErrorCode::conflict, "batch assessment owner or identity mismatch");
    same(state(value.declaration().batch().id()), value.declaration().batch());
    return value;
}

std::vector<proto::BatchAssessment>
Batches::assessments(const std::string& batch, const std::string& after, unsigned limit) const
{
    if (!batch.empty())
        validate_id(batch);
    if (!after.empty())
        validate_id(after);
    if (!limit || limit > 200)
        throw std::invalid_argument("batch assessment page limit must be 1..200");
    std::vector<proto::BatchAssessment> result;
    if (!assessments_present(*database_))
        return result;
    Statement read(
        *database_,
        "SELECT id FROM research_batch_assessments WHERE (?='' OR batch_id=?) AND id>? ORDER BY id LIMIT ?");
    read.bind(1, batch).bind(2, batch).bind(3, after).bind(4, limit);
    while (read.step())
        result.push_back(assessment(read.text(0)));
    return result;
}

std::optional<proto::BatchAssessment> Batches::assessment_receipt(
    const proto::BatchAssessmentMutation& command) const
{
    protocol::validate_message(command);
    validate_id(command.id());
    protocol::validate_batch_assessment_declaration(command.save());
    if (!assessments_present(*database_))
        return std::nullopt;
    Statement read(
        *database_,
        "SELECT request,assessment_id FROM research_batch_assessment_commands WHERE id=?");
    read.bind(1, command.id());
    if (!read.step())
        return std::nullopt;
    if (read.blob(0) != command.SerializeAsString())
        throw Error(
            ErrorCode::conflict,
            "batch assessment request id reused with different content");
    const auto value = assessment(read.text(1));
    if (value.declaration().SerializeAsString() != command.save().SerializeAsString())
        throw Error(ErrorCode::conflict, "batch assessment receipt differs from fixed declaration");
    return value;
}

void Batches::verify_assessment_snapshot(const proto::BatchAssessmentSnapshot& snapshot) const
{
    protocol::validate_batch_assessment_snapshot(snapshot);
    if (snapshot.batch().definition().owner().SerializeAsString() != owner_.SerializeAsString()
        || state(snapshot.batch().definition().request().id()).SerializeAsString()
            != snapshot.batch().SerializeAsString())
        throw Error(
            ErrorCode::conflict,
            "batch assessment observation changed; read a new preview");
    assessment_current(*database_, snapshot.source());
    const auto actual_members = member_tasks(snapshot.batch());
    if (actual_members.size() != static_cast<std::size_t>(snapshot.members_size()))
        throw Error(
            ErrorCode::conflict,
            "batch assessment observation changed; read a new preview");
    for (const auto& member : snapshot.members())
        assessment_current(*database_, member);
}

proto::BatchAssessment Batches::save_assessment(
    const proto::BatchAssessmentMutation& command,
    const proto::BatchAssessmentSnapshot& snapshot,
    const std::string& recording_program)
{
    writable();
    protocol::validate_message(command);
    validate_id(command.id());
    protocol::validate_batch_assessment_declaration(command.save());
    Database::Transaction transaction(*database_);
    if (const auto previous = assessment_receipt(command))
        return *previous;
    verify_assessment_snapshot(snapshot);
    if (command.save().batch().SerializeAsString()
            != protocol::backtest_batch_object(snapshot.batch().definition()).SerializeAsString()
        || command.save().expected_snapshot()
            != protocol::batch_assessment_snapshot_digest(snapshot))
        throw Error(
            ErrorCode::conflict,
            "batch assessment observation changed; read a new preview");
    proto::BatchAssessment value;
    *value.mutable_declaration() = command.save();
    *value.mutable_snapshot() = snapshot;
    *value.mutable_owner() = owner_;
    value.set_program_artifact(recording_program);
    value.set_recorded_at_ms(clock_->utc_now() / 1000000);
    value.set_digest(protocol::batch_assessment_digest(value));
    protocol::validate_batch_assessment(value);
    if (!value.declaration().previous_id().empty()
        && assessment(value.declaration().previous_id()).declaration().batch().SerializeAsString()
            != value.declaration().batch().SerializeAsString())
        throw Error(ErrorCode::conflict, "batch assessment revision belongs to another manifest");
    if (!assessments_present(*database_)) {
        database_->execute(
            "CREATE TABLE research_batch_assessments(id TEXT PRIMARY KEY,batch_id TEXT NOT NULL,body BLOB NOT NULL) STRICT");
        database_->execute(
            "CREATE INDEX research_batch_assessments_by_batch ON research_batch_assessments(batch_id,id)");
        database_->execute(
            "CREATE TABLE research_batch_assessment_commands(id TEXT PRIMARY KEY,request BLOB NOT NULL,assessment_id TEXT NOT NULL) STRICT");
    }
    Statement existing(*database_, "SELECT id FROM research_batch_assessments WHERE id=?");
    existing.bind(1, value.declaration().id());
    if (existing.step())
        throw Error(ErrorCode::conflict, "batch assessment identity is already used");
    Statement save(*database_, "INSERT INTO research_batch_assessments VALUES(?,?,?)");
    save.bind(1, value.declaration().id())
        .bind(2, value.declaration().batch().id())
        .bind_blob(3, value.SerializeAsString())
        .step();
    Statement receipt(*database_, "INSERT INTO research_batch_assessment_commands VALUES(?,?,?)");
    receipt.bind(1, command.id())
        .bind_blob(2, command.SerializeAsString())
        .bind(3, value.declaration().id())
        .step();
    transaction.commit();
    return value;
}

std::vector<std::string> Batches::assessment_consumers(const std::string& task) const
{
    // Retirement resolves canonical formal references, never a rebuildable
    // resource index or a second persisted copy of the reference list.
    std::vector<std::string> result;
    std::string after;
    for (;;) {
        const auto page = assessments({}, after, 200);
        if (page.empty())
            break;
        for (const auto& value : page) {
            if (assessment_sources(value).contains(task))
                result.push_back(value.declaration().id());
            after = value.declaration().id();
        }
    }
    return result;
}

void Batches::inspect_assessments(
    const std::filesystem::path& root,
    const resource::proto::Owner& expected,
    const std::function<void(const proto::BatchAssessment&)>& visitor,
    std::stop_token stop)
{
    owner(expected);
    FileLock guard(root, "manager.lock", FileLock::Access::shared_existing);
    Database database(root / "tasks.sqlite", Database::Access::cold_read_only);
    Batches view(
        database,
        expected,
        Database::Access::cold_read_only,
        std::make_shared<SystemClock>());
    if (!assessments_present(database))
        return;
    std::string after;
    for (;;) {
        const auto page = view.assessments({}, after, 200);
        if (page.empty())
            break;
        for (const auto& value : page) {
            if (stop.stop_requested())
                throw Error(ErrorCode::cancelled, "batch assessment cold inspection interrupted");
            same(view.state(value.declaration().batch().id()), value.declaration().batch());
            for (const auto& id : assessment_sources(value)) {
                const auto actual = record(database, id);
                if (!actual)
                    throw Error(
                        ErrorCode::conflict,
                        "batch assessment retained source task is missing");
                const auto& observed = id == value.snapshot().source().id()
                    ? value.snapshot().source()
                    : *std::ranges::find(
                          value.snapshot().members(),
                          id,
                          &proto::BatchAssessmentTask::id);
                if (actual->input_digest() != observed.input_sha256()
                    || actual->task().attempt() < observed.attempt()
                    || (observed.state() == task::proto::SUCCEEDED
                        && actual->task().result_digest() != observed.result_sha256()))
                    throw Error(
                        ErrorCode::conflict,
                        "batch assessment retained source identity changed");
            }
            if (!value.declaration().previous_id().empty())
                (void)view.assessment(value.declaration().previous_id());
            visitor(value);
            after = value.declaration().id();
        }
    }
    Statement commands(
        database,
        "SELECT id,CASE WHEN length(request)<=131072 THEN request END,assessment_id FROM research_batch_assessment_commands ORDER BY id");
    while (commands.step()) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "batch assessment cold inspection interrupted");
        proto::BatchAssessmentMutation command;
        if (!command.ParseFromString(commands.blob(1)) || command.id() != commands.text(0))
            throw std::invalid_argument("invalid batch assessment receipt");
        const auto receipt = view.assessment_receipt(command);
        if (!receipt || receipt->declaration().id() != commands.text(2))
            throw std::invalid_argument("invalid batch assessment receipt");
    }
}

resource::proto::Catalogue Batches::resources(const resource::proto::CatalogueQuery& query) const
{
    const auto limit = protocol::catalogue_limit(query);
    if (query.kind() == "backtest_batch_assessment") {
        resource::proto::Catalogue result;
        *result.mutable_owner() = owner_;
        result.set_kind(query.kind());
        const auto page = assessments({}, query.after(), limit);
        for (const auto& value : page) {
            auto& entry = *result.add_items();
            *entry.mutable_object() = protocol::batch_assessment_object(value);
            entry.set_name(value.declaration().question());
            entry.set_state(resource::proto::CatalogueEntry::SEALED);
            entry.set_relative_location("tasks.sqlite");
            entry.set_logical_bytes(value.ByteSizeLong());
        }
        if (page.size() == limit && !assessments({}, page.back().declaration().id(), 1).empty())
            result.set_next_after(page.back().declaration().id());
        return result;
    }
    if (query.kind() != "backtest_batch")
        throw Error(ErrorCode::invalid_request, "resource kind is not owned by research batches");
    resource::proto::Catalogue result;
    *result.mutable_owner() = owner_;
    result.set_kind(query.kind());
    const auto page = states(query.after(), limit);
    for (const auto& value : page) {
        auto& entry = *result.add_items();
        *entry.mutable_object() = protocol::backtest_batch_object(value.definition());
        entry.set_name(value.definition().request().name());
        entry.set_state(resource::proto::CatalogueEntry::SEALED);
        entry.set_relative_location("tasks.sqlite");
        entry.set_logical_bytes(value.ByteSizeLong());
    }
    if (page.size() == limit && !states(page.back().definition().request().id(), 1).empty())
        result.set_next_after(page.back().definition().request().id());
    return result;
}

void Batches::inspect_backup(
    const std::filesystem::path& root,
    const resource::proto::Owner& expected,
    std::span<const task::proto::StoredTaskRecord> required,
    const std::function<void(const proto::BacktestBatchState&)>& visitor,
    std::stop_token stop)
{
    owner(expected);
    FileLock guard(root, "manager.lock", FileLock::Access::shared_existing);
    Database database(root / "tasks.sqlite", Database::Access::cold_read_only);
    Batches view(
        database,
        expected,
        Database::Access::cold_read_only,
        std::make_shared<SystemClock>());
    const auto check = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "research batch cold inspection interrupted");
    };
    for (const auto& task : required) {
        check();
        protocol::validate_message(task);
        const auto& object = task.task().metadata().derivation().batch();
        const auto state = view.state(object.id());
        same(state, object);
        verify_member_record(state, member(state, task.task().id()), task);
    }
    std::string after;
    for (;;) {
        check();
        const auto page = view.states(after, 200);
        if (page.empty())
            break;
        for (const auto& value : page) {
            check();
            view.verify_source(value.definition(), false);
            (void)view.member_tasks(value);
            visitor(value);
        }
        after = page.back().definition().request().id();
    }
    if (!inspect(database, expected))
        return;
    Statement members(
        database,
        "SELECT task_id,batch_id FROM research_batch_members ORDER BY task_id");
    while (members.step()) {
        check();
        (void)member(view.state(members.text(1)), members.text(0));
    }
    Statement receipts(
        database,
        "SELECT id,CASE WHEN length(request)<=65536 THEN request END,CASE WHEN length(response)<=262144 THEN response END FROM research_batch_commands ORDER BY id");
    while (receipts.step()) {
        check();
        proto::BacktestBatchMutation command;
        proto::BacktestBatchState response;
        if (!command.ParseFromString(receipts.blob(1))
            || !response.ParseFromString(receipts.blob(2)) || command.id() != receipts.text(0))
            throw std::invalid_argument("invalid research batch receipt");
        protocol::validate_backtest_batch_mutation(command);
        receipt_shape(command, response, expected);
        if (view.state(response.definition().request().id()).definition().SerializeAsString()
            != response.definition().SerializeAsString())
            throw Error(ErrorCode::conflict, "batch receipt differs from immutable definition");
    }
}

} // namespace asterion::research
