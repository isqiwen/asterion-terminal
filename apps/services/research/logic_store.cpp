#include "logic_store.hpp"
#include "task_store.hpp"
#include <asterion/protocol/research_logic.hpp>
#include <asterion/protocol/resource.hpp>
#include <asterion/protocol/trading.hpp>
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
        throw std::invalid_argument("research logic owner required");
}

bool inspect(Database& database, const resource::proto::Owner& expected)
{
    Statement tasks(database, "SELECT id FROM tasks LIMIT 1");
    (void)tasks.step();
    Statement tables(
        database,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('research_logic_owner','research_logic_definitions','research_logic_examples','research_logic_commands')");
    (void)tables.step();
    const auto count = tables.integer(0);
    if (!count)
        return false;
    if (count != 4)
        throw std::invalid_argument("incomplete research logic schema");
    Statement read(
        database,
        "SELECT CASE WHEN length(body)<=4096 THEN body END FROM research_logic_owner WHERE singleton=1");
    resource::proto::Owner actual;
    if (!read.step() || !actual.ParseFromString(read.blob(0)))
        throw std::invalid_argument("invalid research logic owner");
    owner(actual);
    if (actual.SerializeAsString() != expected.SerializeAsString())
        throw Error(ErrorCode::conflict, "research logic owner mismatch");
    return true;
}

void request(const proto::LogicMutation& value)
{
    protocol::validate_message(value);
    validate_id(value.id());
    if (value.ByteSizeLong() > 262144
        || (!value.has_registration() && !value.has_example() && !value.has_retire()))
        throw std::invalid_argument("invalid research logic mutation");
    if (value.has_registration())
        validate_id(value.registration().id());
    else if (value.has_example())
        validate_id(value.example().id());
    else
        protocol::validate_logic_object(value.retire().definition(), "logic_definition");
}

void receipt_shape(
    const proto::LogicMutation& request,
    const proto::LogicReply& reply,
    const resource::proto::Owner& expected)
{
    protocol::validate_message(reply);
    if (reply.ByteSizeLong() > 262144 || reply.has_definition() != !request.has_example()
        || reply.has_example() != request.has_example() || reply.definitions_size()
        || reply.examples_size() || !reply.next_after().empty())
        throw std::invalid_argument("invalid research logic receipt");
    if (reply.has_definition()) {
        const auto& value = reply.definition();
        protocol::validate_logic_definition(value);
        if (value.owner().SerializeAsString() != expected.SerializeAsString())
            throw Error(ErrorCode::conflict, "research logic owner mismatch");
        if (request.has_registration()) {
            proto::LogicRegister original;
            original.set_id(value.id());
            original.set_name(value.name());
            original.set_purpose(value.purpose());
            original.set_fast(value.fast());
            original.set_slow(value.slow());
            original.set_input(value.input());
            original.set_output(value.output());
            if (!value.retirement_reason().empty()
                || original.SerializeAsString() != request.registration().SerializeAsString())
                throw std::invalid_argument("invalid research logic receipt");
        } else if (value.retirement_reason().empty()
            || value.retirement_reason() != request.retire().reason()
            || protocol::logic_object(value).SerializeAsString()
                != request.retire().definition().SerializeAsString())
            throw std::invalid_argument("invalid research logic receipt");
    } else {
        const auto& value = reply.example();
        protocol::validate_logic_example(value);
        if (value.owner().SerializeAsString() != expected.SerializeAsString())
            throw Error(ErrorCode::conflict, "research logic owner mismatch");
        proto::LogicExampleSave original;
        original.set_id(value.id());
        *original.mutable_definition() = value.definition();
        original.set_name(value.name());
        original.set_use_scope(value.use_scope());
        original.set_fictional(value.fictional());
        *original.mutable_observations() = value.observations();
        if (original.SerializeAsString() != request.example().SerializeAsString())
            throw std::invalid_argument("invalid research logic receipt");
    }
}

void page(const std::string& after, unsigned limit)
{
    if (!after.empty())
        validate_id(after);
    if (!limit || limit > 200)
        throw std::invalid_argument("research logic page limit must be 1..200");
}

} // namespace

Logic::Logic(
    tasks::Store& store,
    resource::proto::Owner expected,
    Database::Access access,
    std::shared_ptr<const Clock> clock)
    : Logic(
          store.database(expected.service_id(), access == Database::Access::writer),
          expected,
          access,
          std::move(clock))
{}

Logic::Logic(
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
    if (writer_ && !inspect(database, owner_)) {
        Database::Transaction transaction(database);
        database.execute(
            "CREATE TABLE research_logic_owner(singleton INTEGER PRIMARY KEY CHECK(singleton=1),body BLOB NOT NULL) STRICT");
        database.execute(
            "CREATE TABLE research_logic_definitions(id TEXT PRIMARY KEY,retired INTEGER NOT NULL CHECK(retired IN (0,1)),body BLOB NOT NULL) STRICT");
        database.execute(
            "CREATE TABLE research_logic_examples(id TEXT PRIMARY KEY,definition_id TEXT NOT NULL,body BLOB NOT NULL) STRICT");
        database.execute(
            "CREATE INDEX research_logic_example_definition ON research_logic_examples(definition_id,id)");
        database.execute(
            "CREATE TABLE research_logic_commands(id TEXT PRIMARY KEY,request BLOB NOT NULL,response BLOB NOT NULL) STRICT");
        Statement own(database, "INSERT INTO research_logic_owner VALUES(1,?)");
        own.bind_blob(1, owner_.SerializeAsString()).step();
        transaction.commit();
    } else if (!writer_)
        (void)inspect(database, owner_);
}

std::optional<proto::LogicReply> Logic::receipt(const proto::LogicMutation& mutation) const
{
    request(mutation);
    if (!inspect(*database_, owner_))
        return std::nullopt;
    Statement read(
        *database_,
        "SELECT CASE WHEN length(request)<=262144 THEN request END,CASE WHEN length(response)<=262144 THEN response END FROM research_logic_commands WHERE id=?");
    read.bind(1, mutation.id());
    if (!read.step())
        return std::nullopt;
    if (read.blob(0) != mutation.SerializeAsString())
        throw Error(ErrorCode::conflict, "logic request id reused with different content");
    proto::LogicReply reply;
    if (!reply.ParseFromString(read.blob(1)))
        throw std::invalid_argument("invalid research logic receipt");
    receipt_shape(mutation, reply, owner_);
    return reply;
}

proto::LogicDefinition Logic::definition(const std::string& id) const
{
    validate_id(id);
    if (!inspect(*database_, owner_))
        throw Error(ErrorCode::not_found, "unknown research logic definition");
    Statement read(
        *database_,
        "SELECT retired,CASE WHEN length(body)<=16384 THEN body END FROM research_logic_definitions WHERE id=?");
    read.bind(1, id);
    if (!read.step())
        throw Error(ErrorCode::not_found, "unknown research logic definition");
    proto::LogicDefinition value;
    if (!value.ParseFromString(read.blob(1)))
        throw std::invalid_argument("invalid research logic definition");
    protocol::validate_logic_definition(value);
    if (value.id() != id || value.owner().SerializeAsString() != owner_.SerializeAsString()
        || (read.integer(0) != 0) != !value.retirement_reason().empty())
        throw Error(ErrorCode::conflict, "research logic definition index mismatch");
    return value;
}

proto::LogicExample Logic::example(const std::string& id) const
{
    validate_id(id);
    if (!inspect(*database_, owner_))
        throw Error(ErrorCode::not_found, "unknown research logic example");
    Statement read(
        *database_,
        "SELECT definition_id,CASE WHEN length(body)<=131072 THEN body END FROM research_logic_examples WHERE id=?");
    read.bind(1, id);
    if (!read.step())
        throw Error(ErrorCode::not_found, "unknown research logic example");
    proto::LogicExample value;
    if (!value.ParseFromString(read.blob(1)))
        throw std::invalid_argument("invalid fictional logic example");
    protocol::validate_logic_example(value);
    if (value.id() != id || value.owner().SerializeAsString() != owner_.SerializeAsString()
        || value.definition().id() != read.text(0)
        || value.definition().SerializeAsString()
            != protocol::logic_object(definition(read.text(0))).SerializeAsString())
        throw Error(ErrorCode::conflict, "research logic example definition mismatch");
    return value;
}

std::vector<proto::LogicDefinition>
Logic::definitions(const std::string& after, unsigned limit) const
{
    page(after, limit);
    std::vector<proto::LogicDefinition> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement rows(
        *database_,
        "SELECT id FROM research_logic_definitions WHERE id>? ORDER BY id LIMIT ?");
    rows.bind(1, after).bind(2, static_cast<std::int64_t>(limit));
    while (rows.step())
        result.push_back(definition(rows.text(0)));
    return result;
}

std::vector<proto::LogicExample>
Logic::examples(const std::string& definition_id, const std::string& after, unsigned limit) const
{
    page(after, limit);
    if (!definition_id.empty())
        validate_id(definition_id);
    std::vector<proto::LogicExample> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement rows(
        *database_,
        "SELECT id FROM research_logic_examples WHERE (?='' OR definition_id=?) AND id>? ORDER BY id LIMIT ?");
    rows.bind(1, definition_id)
        .bind(2, definition_id)
        .bind(3, after)
        .bind(4, static_cast<std::int64_t>(limit));
    while (rows.step())
        result.push_back(example(rows.text(0)));
    return result;
}

proto::LogicReply Logic::apply(const proto::LogicMutation& mutation)
{
    if (!writer_)
        throw Error(ErrorCode::permission_denied, "research logic access is read only");
    request(mutation);
    Database::Transaction transaction(*database_);
    if (const auto original = receipt(mutation))
        return *original;
    proto::LogicReply reply;
    if (mutation.has_registration()) {
        const auto& input = mutation.registration();
        auto& value = *reply.mutable_definition();
        value.set_id(input.id());
        *value.mutable_owner() = owner_;
        value.set_name(input.name());
        value.set_purpose(input.purpose());
        value.set_fast(input.fast());
        value.set_slow(input.slow());
        value.set_input(input.input());
        value.set_output(input.output());
        value.set_recorded_at_ms(clock_->utc_now() / 1000000);
        value.set_digest(protocol::logic_digest(value));
        receipt_shape(mutation, reply, owner_);
        Statement existing(*database_, "SELECT 1 FROM research_logic_definitions WHERE id=?");
        existing.bind(1, value.id());
        if (existing.step())
            throw Error(ErrorCode::conflict, "research logic definition already exists");
        Statement persist(*database_, "INSERT INTO research_logic_definitions VALUES(?,0,?)");
        persist.bind(1, value.id()).bind_blob(2, value.SerializeAsString()).step();
    } else if (mutation.has_example()) {
        const auto& input = mutation.example();
        const auto fixed = definition(input.definition().id());
        if (!fixed.retirement_reason().empty())
            throw Error(ErrorCode::conflict, "research logic definition is deprecated");
        auto& value = *reply.mutable_example();
        value.set_id(input.id());
        *value.mutable_owner() = owner_;
        *value.mutable_definition() = input.definition();
        value.set_name(input.name());
        value.set_use_scope(input.use_scope());
        value.set_fictional(input.fictional());
        *value.mutable_observations() = input.observations();
        value.set_recorded_at_ms(clock_->utc_now() / 1000000);
        value.set_digest(protocol::logic_example_digest(value));
        receipt_shape(mutation, reply, owner_);
        if (value.definition().SerializeAsString()
            != protocol::logic_object(fixed).SerializeAsString())
            throw Error(ErrorCode::conflict, "research logic example definition mismatch");
        Statement existing(*database_, "SELECT 1 FROM research_logic_examples WHERE id=?");
        existing.bind(1, value.id());
        if (existing.step())
            throw Error(ErrorCode::conflict, "research logic example already exists");
        Statement persist(*database_, "INSERT INTO research_logic_examples VALUES(?,?,?)");
        persist.bind(1, value.id())
            .bind(2, fixed.id())
            .bind_blob(3, value.SerializeAsString())
            .step();
    } else {
        auto value = definition(mutation.retire().definition().id());
        if (protocol::logic_object(value).SerializeAsString()
            != mutation.retire().definition().SerializeAsString())
            throw Error(ErrorCode::conflict, "research logic identity changed");
        if (!value.retirement_reason().empty()
            && value.retirement_reason() != mutation.retire().reason())
            throw Error(ErrorCode::conflict, "logic deprecation reason is already fixed");
        value.set_retirement_reason(mutation.retire().reason());
        *reply.mutable_definition() = value;
        receipt_shape(mutation, reply, owner_);
        Statement persist(
            *database_,
            "UPDATE research_logic_definitions SET retired=1,body=? WHERE id=?");
        persist.bind_blob(1, value.SerializeAsString()).bind(2, value.id()).step();
    }
    Statement receipt_write(*database_, "INSERT INTO research_logic_commands VALUES(?,?,?)");
    receipt_write.bind(1, mutation.id())
        .bind_blob(2, mutation.SerializeAsString())
        .bind_blob(3, reply.SerializeAsString())
        .step();
    transaction.commit();
    return reply;
}

proto::LogicTrialInput Logic::input(const resource::proto::Object& selected, bool admission) const
{
    protocol::validate_logic_object(selected, "logic_case");
    const auto samples = example(selected.id());
    if (protocol::logic_object(samples).SerializeAsString() != selected.SerializeAsString())
        throw Error(ErrorCode::conflict, "logic trial example identity changed");
    proto::LogicTrialInput value;
    *value.mutable_owner() = owner_;
    *value.mutable_example() = samples;
    *value.mutable_definition() = definition(samples.definition().id());
    if (admission && !value.definition().retirement_reason().empty())
        throw Error(ErrorCode::conflict, "research logic definition is deprecated");
    protocol::validate_logic_trial_input(value);
    return value;
}

void Logic::verify_input(const proto::LogicTrialInput& fixed, bool admission) const
{
    protocol::validate_logic_trial_input(fixed);
    const auto current = input(protocol::logic_object(fixed.example()), admission);
    auto original_definition = fixed.definition();
    original_definition.clear_retirement_reason();
    auto current_definition = current.definition();
    current_definition.clear_retirement_reason();
    if (fixed.owner().SerializeAsString() != owner_.SerializeAsString()
        || current.example().SerializeAsString() != fixed.example().SerializeAsString()
        || original_definition.SerializeAsString() != current_definition.SerializeAsString())
        throw Error(ErrorCode::conflict, "logic trial fixed metadata mismatch");
}

resource::proto::Catalogue Logic::resources(const resource::proto::CatalogueQuery& query) const
{
    const auto limit = protocol::catalogue_limit(query);
    resource::proto::Catalogue result;
    *result.mutable_owner() = owner_;
    result.set_kind(query.kind());
    if (query.kind() == "logic_definition") {
        const auto rows = definitions(query.after(), limit);
        for (const auto& value : rows) {
            auto& entry = *result.add_items();
            *entry.mutable_object() = protocol::logic_object(value);
            entry.set_name(value.name());
            entry.set_state(
                value.retirement_reason().empty() ? resource::proto::CatalogueEntry::SEALED
                                                  : resource::proto::CatalogueEntry::RETIRED);
            entry.set_relative_location("tasks.sqlite");
            entry.set_logical_bytes(value.ByteSizeLong());
        }
        if (rows.size() == limit && !definitions(rows.back().id(), 1).empty())
            result.set_next_after(rows.back().id());
    } else if (query.kind() == "logic_case") {
        const auto rows = examples("", query.after(), limit);
        for (const auto& value : rows) {
            auto& entry = *result.add_items();
            *entry.mutable_object() = protocol::logic_object(value);
            entry.set_name(value.name());
            entry.set_state(resource::proto::CatalogueEntry::SEALED);
            entry.set_relative_location("tasks.sqlite");
            entry.set_logical_bytes(value.ByteSizeLong());
        }
        if (rows.size() == limit && !examples("", rows.back().id(), 1).empty())
            result.set_next_after(rows.back().id());
    } else
        throw Error(ErrorCode::invalid_request, "resource kind is not implemented by Research");
    return result;
}

void Logic::inspect_backup(
    const std::filesystem::path& root,
    const resource::proto::Owner& expected,
    std::span<const proto::LogicTrialInput> required,
    const std::function<void(const proto::LogicDefinition&)>& definitions_visitor,
    const std::function<void(const proto::LogicExample&)>& examples_visitor,
    std::stop_token stop)
{
    owner(expected);
    FileLock guard(root, "manager.lock", FileLock::Access::shared_existing);
    Database database(root / "tasks.sqlite", Database::Access::cold_read_only);
    Logic view(
        database,
        expected,
        Database::Access::cold_read_only,
        std::make_shared<SystemClock>());
    const auto check = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "research logic cold inspection interrupted");
    };
    for (const auto& input : required) {
        check();
        view.verify_input(input, false);
    }
    std::string after;
    for (;;) {
        check();
        const auto rows = view.definitions(after, 200);
        if (rows.empty())
            break;
        for (const auto& value : rows) {
            check();
            definitions_visitor(value);
        }
        after = rows.back().id();
    }
    after.clear();
    for (;;) {
        check();
        const auto rows = view.examples("", after, 200);
        if (rows.empty())
            break;
        for (const auto& value : rows) {
            check();
            examples_visitor(value);
        }
        after = rows.back().id();
    }
    if (!inspect(database, expected))
        return;
    Statement receipts(
        database,
        "SELECT id,CASE WHEN length(request)<=262144 THEN request END,CASE WHEN length(response)<=262144 THEN response END FROM research_logic_commands ORDER BY id");
    while (receipts.step()) {
        check();
        proto::LogicMutation request_value;
        proto::LogicReply response;
        if (!request_value.ParseFromString(receipts.blob(1))
            || !response.ParseFromString(receipts.blob(2))
            || request_value.id() != receipts.text(0))
            throw std::invalid_argument("invalid research logic receipt");
        request(request_value);
        receipt_shape(request_value, response, expected);
        if (response.has_example()) {
            if (response.example().SerializeAsString()
                != view.example(response.example().id()).SerializeAsString())
                throw Error(ErrorCode::conflict, "logic receipt differs from immutable example");
        } else {
            auto original = response.definition();
            auto current = view.definition(original.id());
            original.clear_retirement_reason();
            current.clear_retirement_reason();
            if (original.SerializeAsString() != current.SerializeAsString())
                throw Error(ErrorCode::conflict, "logic receipt differs from immutable definition");
        }
    }
}

} // namespace asterion::research
