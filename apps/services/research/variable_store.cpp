#include "variable_store.hpp"
#include "task_store.hpp"
#include "feature_expression.hpp"
#include <asterion/protocol/research_variable.hpp>
#include <asterion/protocol/resource.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <algorithm>
#include <cctype>

namespace asterion::research {
namespace {

using Database = sqlite::Database;
using Statement = Database::Statement;

bool blank(const std::string& s)
{
    return std::ranges::all_of(s, [](unsigned char c) {
        return std::isspace(c);
    });
}

void validate_owner(const resource::proto::Owner& value)
{
    protocol::validate_message(value);
    validate_id(value.node_id());
    validate_id(value.service_id());
    if (value.domain() != "research")
        throw std::invalid_argument("research variable owner required");
}

bool inspect(Database& database, const resource::proto::Owner& expected)
{
    {
        Statement tasks(database, "SELECT id FROM tasks LIMIT 1");
        (void)tasks.step();
    }
    Statement tables(
        database,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('research_variable_owner','research_variable_definitions','research_variable_commands')");
    (void)tables.step();
    const auto count = tables.integer(0);
    if (!count)
        return false;
    if (count != 3)
        throw std::invalid_argument("incomplete research variable schema");
    Statement owned(
        database,
        "SELECT CASE WHEN length(body)<=4096 THEN body END FROM research_variable_owner WHERE singleton=1");
    resource::proto::Owner actual;
    if (!owned.step() || !actual.ParseFromString(owned.blob(0)))
        throw std::invalid_argument("invalid research variable owner");
    validate_owner(actual);
    if (actual.SerializeAsString() != expected.SerializeAsString())
        throw Error(ErrorCode::conflict, "research variable owner mismatch");
    return true;
}

void validate(const proto::VariableDefinition& value, const resource::proto::Owner& expected)
{
    protocol::validate_variable_definition(value);
    if (value.owner().SerializeAsString() != expected.SerializeAsString())
        throw Error(ErrorCode::conflict, "research variable owner mismatch");
    const FeatureExpression expression(value.expression());
    if (expression.price_power() != value.price_power()
        || expression.history(value.window()) != value.required_history())
        throw std::invalid_argument("research variable semantics mismatch");
}

void receipt(
    const proto::VariableMutation& request,
    const proto::VariableReply& reply,
    const resource::proto::Owner& expected)
{
    protocol::validate_message(request);
    protocol::validate_message(reply);
    validate_id(request.id());
    if (!request.has_registration() || !reply.has_definition() || reply.definitions_size()
        || reply.has_preview() || !reply.next_after().empty() || request.ByteSizeLong() > 32768
        || reply.ByteSizeLong() > 32768)
        throw std::invalid_argument("invalid research variable receipt");
    validate(reply.definition(), expected);
    const auto& source = request.registration();
    const auto& value = reply.definition();
    if (source.id() != value.id() || source.name() != value.name()
        || source.purpose() != value.purpose() || source.expression() != value.expression()
        || source.window() != value.window())
        throw std::invalid_argument("invalid research variable receipt");
}

} // namespace

Variables::Variables(
    tasks::Store& store,
    resource::proto::Owner expected,
    Database::Access access,
    std::shared_ptr<const Clock> clock)
    : Variables(
          store.database(expected.service_id(), access == Database::Access::writer),
          expected,
          access,
          std::move(clock))
{}

Variables::Variables(
    Database& database,
    resource::proto::Owner expected,
    Database::Access access,
    std::shared_ptr<const Clock> clock)
    : owner_(std::move(expected)),
      clock_(std::move(clock)),
      database_(&database),
      writer_(access == Database::Access::writer)
{
    validate_owner(owner_);
    if (writer_ && !inspect(database, owner_)) {
        Database::Transaction transaction(database);
        database.execute(
            "CREATE TABLE research_variable_owner(singleton INTEGER PRIMARY KEY CHECK(singleton=1),body BLOB NOT NULL) STRICT");
        database.execute(
            "CREATE TABLE research_variable_definitions(id TEXT PRIMARY KEY,body BLOB NOT NULL) STRICT");
        database.execute(
            "CREATE TABLE research_variable_commands(id TEXT PRIMARY KEY,request BLOB NOT NULL,response BLOB NOT NULL) STRICT");
        Statement owned(database, "INSERT INTO research_variable_owner VALUES(1,?)");
        owned.bind_blob(1, owner_.SerializeAsString()).step();
        transaction.commit();
    } else if (!writer_)
        (void)inspect(database, owner_);
}

proto::VariableDefinition Variables::definition(const std::string& id) const
{
    validate_id(id);
    if (!inspect(*database_, owner_))
        throw Error(ErrorCode::not_found, "unknown research variable definition");
    Statement read(
        *database_,
        "SELECT CASE WHEN length(body)<=32768 THEN body END FROM research_variable_definitions WHERE id=?");
    read.bind(1, id);
    if (!read.step())
        throw Error(ErrorCode::not_found, "unknown research variable definition");
    proto::VariableDefinition value;
    if (!value.ParseFromString(read.blob(0)) || value.id() != id)
        throw std::invalid_argument("invalid research variable definition");
    validate(value, owner_);
    return value;
}

std::vector<proto::VariableDefinition>
Variables::list(const std::string& after, unsigned limit) const
{
    if (!after.empty())
        validate_id(after);
    if (!limit || limit > 200)
        throw std::invalid_argument("invalid research variable page size");
    std::vector<proto::VariableDefinition> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement rows(
        *database_,
        "SELECT id FROM research_variable_definitions WHERE id>? ORDER BY id LIMIT ?");
    rows.bind(1, after).bind(2, static_cast<std::int64_t>(limit));
    while (rows.step())
        result.push_back(definition(rows.text(0)));
    return result;
}

proto::VariablePreview Variables::preview(const proto::VariableRegister& input)
{
    protocol::validate_message(input);
    proto::VariablePreview result;
    try {
        const FeatureExpression expression(input.expression());
        result.set_required_history(static_cast<unsigned>(expression.history(input.window())));
        result.set_price_power(expression.price_power());
        result.set_valid(true);
    } catch (const FeatureExpressionError& error) {
        result.set_error_offset(static_cast<unsigned>(error.position()));
        result.set_diagnostic(error.reason());
    }
    return result;
}

proto::VariableReply Variables::apply(const proto::VariableMutation& request)
{
    if (!writer_)
        throw Error(ErrorCode::permission_denied, "research variable access is read only");
    protocol::validate_message(request);
    validate_id(request.id());
    if (!request.has_registration() || request.ByteSizeLong() > 32768)
        throw std::invalid_argument("invalid research variable registration");
    const auto bytes = request.SerializeAsString();
    Database::Transaction transaction(*database_);
    {
        Statement replay(
            *database_,
            "SELECT CASE WHEN length(request)<=32768 THEN request END,CASE WHEN length(response)<=32768 THEN response END FROM research_variable_commands WHERE id=?");
        replay.bind(1, request.id());
        if (replay.step()) {
            if (replay.blob(0) != bytes)
                throw Error(
                    ErrorCode::conflict,
                    "variable request id reused with different content");
            proto::VariableReply result;
            if (!result.ParseFromString(replay.blob(1)))
                throw std::invalid_argument("invalid research variable receipt");
            receipt(request, result, owner_);
            return result;
        }
    }
    const auto& input = request.registration();
    validate_id(input.id());
    if (blank(input.name()) || input.name().size() > 256)
        throw std::invalid_argument("research variable name required or exceeds limit");
    if (blank(input.purpose()) || input.purpose().size() > 4096)
        throw std::invalid_argument("research variable purpose required or exceeds limit");
    const FeatureExpression expression(input.expression());
    const auto history = expression.history(input.window());
    Statement existing(*database_, "SELECT 1 FROM research_variable_definitions WHERE id=?");
    existing.bind(1, input.id());
    if (existing.step())
        throw Error(ErrorCode::conflict, "research variable definition already exists");
    proto::VariableReply result;
    auto& value = *result.mutable_definition();
    value.set_id(input.id());
    *value.mutable_owner() = owner_;
    value.set_name(input.name());
    value.set_purpose(input.purpose());
    value.set_expression(input.expression());
    value.set_window(input.window());
    value.set_price_power(expression.price_power());
    value.set_required_history(static_cast<unsigned>(history));
    value.set_recorded_at_ms(clock_->utc_now() / 1000000);
    value.set_digest(protocol::variable_digest(value));
    receipt(request, result, owner_);
    Statement persist(*database_, "INSERT INTO research_variable_definitions(id,body) VALUES(?,?)");
    persist.bind(1, value.id()).bind_blob(2, value.SerializeAsString()).step();
    Statement record(
        *database_,
        "INSERT INTO research_variable_commands(id,request,response) VALUES(?,?,?)");
    record.bind(1, request.id())
        .bind_blob(2, bytes)
        .bind_blob(3, result.SerializeAsString())
        .step();
    transaction.commit();
    return result;
}

void Variables::verify(const resource::proto::Object& object) const
{
    protocol::verify_variable_reference(object, definition(object.id()));
}

resource::proto::Catalogue Variables::resources(const resource::proto::CatalogueQuery& query) const
{
    const auto limit = protocol::catalogue_limit(query);
    if (query.kind() != "research_variable")
        throw Error(ErrorCode::invalid_request, "resource kind is not implemented by Research");
    resource::proto::Catalogue result;
    *result.mutable_owner() = owner_;
    result.set_kind(query.kind());
    const auto rows = list(query.after(), limit);
    for (const auto& value : rows) {
        auto& row = *result.add_items();
        *row.mutable_object() = protocol::variable_object(value);
        row.set_name(value.name());
        row.set_state(resource::proto::CatalogueEntry::SEALED);
        row.set_logical_bytes(value.ByteSizeLong());
        row.set_relative_location("tasks.sqlite");
    }
    if (rows.size() == limit && !list(rows.back().id(), 1).empty())
        result.set_next_after(rows.back().id());
    return result;
}

void Variables::inspect_backup(
    const std::filesystem::path& root,
    const resource::proto::Owner& expected,
    std::span<const resource::proto::Object> required,
    const std::function<void(const proto::VariableDefinition&)>& visitor,
    std::stop_token stop)
{
    validate_owner(expected);
    FileLock ownership(root, "manager.lock", FileLock::Access::shared_existing);
    Database database(root / "tasks.sqlite", Database::Access::cold_read_only);
    Variables view(
        database,
        expected,
        Database::Access::cold_read_only,
        std::make_shared<SystemClock>());
    const auto check = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "Research variable cold inspection interrupted");
    };
    for (const auto& object : required) {
        check();
        view.verify(object);
    }
    std::string cursor;
    for (;;) {
        check();
        const auto rows = view.list(cursor, 200);
        if (rows.empty())
            break;
        for (const auto& value : rows) {
            check();
            visitor(value);
        }
        cursor = rows.back().id();
    }
    if (!inspect(database, expected))
        return;
    Statement commands(
        database,
        "SELECT id,CASE WHEN length(request)<=32768 THEN request END,CASE WHEN length(response)<=32768 THEN response END FROM research_variable_commands ORDER BY id");
    while (commands.step()) {
        check();
        proto::VariableMutation request;
        proto::VariableReply reply;
        if (!request.ParseFromString(commands.blob(1)) || !reply.ParseFromString(commands.blob(2))
            || request.id() != commands.text(0))
            throw std::invalid_argument("invalid research variable receipt");
        receipt(request, reply, expected);
        if (view.definition(reply.definition().id()).SerializeAsString()
            != reply.definition().SerializeAsString())
            throw std::invalid_argument("invalid research variable receipt");
    }
}

} // namespace asterion::research
