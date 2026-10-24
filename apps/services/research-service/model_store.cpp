#include "model_store.hpp"
#include "research_models.hpp"
#include "task_store.hpp"
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/research_model.hpp>
#include <asterion/protocol/resource.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>

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
        throw std::invalid_argument("research model owner required");
}

bool inspect(Database& db, const resource::proto::Owner& expected)
{
    Statement tasks(db, "SELECT id FROM tasks LIMIT 1");
    (void)tasks.step();
    Statement count(
        db,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('research_model_owner','research_model_drafts','research_model_plans','research_model_commands','research_model_mappings','research_model_time_plans')");
    (void)count.step();
    if (!count.integer(0))
        return false;
    if (count.integer(0) != 6)
        throw std::invalid_argument("incomplete research model schema");
    Statement read(
        db,
        "SELECT CASE WHEN length(body)<=4096 THEN body END FROM research_model_owner WHERE singleton=1");
    resource::proto::Owner actual;
    if (!read.step() || !actual.ParseFromString(read.blob(0)))
        throw std::invalid_argument("invalid research model owner");
    owner(actual);
    if (actual.SerializeAsString() != expected.SerializeAsString())
        throw Error(ErrorCode::conflict, "research model owner mismatch");
    return true;
}

void page(const std::string& after, unsigned limit)
{
    if (!after.empty())
        validate_id(after);
    if (!limit || limit > 200)
        throw std::invalid_argument("model page limit must be 1..200");
}

} // namespace

Models::Models(
    tasks::Store& store,
    resource::proto::Owner expected,
    Database::Access access,
    std::shared_ptr<const Clock> clock)
    : Models(
          store.research_metadata(expected.service_id(), access == Database::Access::writer),
          expected,
          access,
          std::move(clock))
{}

Models::Models(
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
            "CREATE TABLE research_model_owner(singleton INTEGER PRIMARY KEY CHECK(singleton=1),body BLOB NOT NULL) STRICT");
        database_->execute(
            "CREATE TABLE research_model_drafts(id TEXT PRIMARY KEY,source_id TEXT NOT NULL,body BLOB NOT NULL) STRICT");
        database_->execute(
            "CREATE INDEX research_model_draft_source ON research_model_drafts(source_id,id)");
        database_->execute(
            "CREATE TABLE research_model_plans(id TEXT PRIMARY KEY,source_id TEXT NOT NULL,body BLOB NOT NULL) STRICT");
        database_->execute(
            "CREATE INDEX research_model_plan_source ON research_model_plans(source_id,id)");
        database_->execute(
            "CREATE TABLE research_model_commands(id TEXT PRIMARY KEY,request BLOB NOT NULL,response BLOB NOT NULL) STRICT");
        database_->execute(
            "CREATE TABLE research_model_mappings(id TEXT PRIMARY KEY,source_id TEXT NOT NULL,body BLOB NOT NULL) STRICT");
        database_->execute(
            "CREATE INDEX research_model_mapping_source ON research_model_mappings(source_id,id)");
        database_->execute(
            "CREATE TABLE research_model_time_plans(id TEXT PRIMARY KEY,source_id TEXT NOT NULL,body BLOB NOT NULL) STRICT");
        database_->execute(
            "CREATE INDEX research_model_time_source ON research_model_time_plans(source_id,id)");
        Statement write(*database_, "INSERT INTO research_model_owner VALUES(1,?)");
        write.bind_blob(1, owner_.SerializeAsString()).step();
        tx.commit();
    } else if (!writer_)
        (void)inspect(*database_, owner_);
}

std::optional<proto::ModelDraft> Models::draft(const std::string& id) const
{
    validate_id(id);
    if (!inspect(*database_, owner_))
        return {};
    Statement read(
        *database_,
        "SELECT source_id,CASE WHEN length(body)<=32768 THEN body END FROM research_model_drafts WHERE id=?");
    read.bind(1, id);
    if (!read.step())
        return {};
    proto::ModelDraft value;
    if (!value.ParseFromString(read.blob(1)))
        throw std::invalid_argument("invalid stored model draft");
    protocol::validate_model_draft(value);
    if (value.id() != id || value.owner().SerializeAsString() != owner_.SerializeAsString()
        || value.specification().source().id() != read.text(0)
        || (value.specification().has_source()
            && value.specification().source().owner().SerializeAsString()
                != owner_.SerializeAsString()))
        throw Error(ErrorCode::conflict, "model draft owner or index mismatch");
    return value;
}

proto::ModelPlan Models::plan(const std::string& id) const
{
    validate_id(id);
    if (!inspect(*database_, owner_))
        throw Error(ErrorCode::not_found, "unknown fixed model plan");
    Statement read(
        *database_,
        "SELECT source_id,CASE WHEN length(body)<=65536 THEN body END FROM research_model_plans WHERE id=?");
    read.bind(1, id);
    if (!read.step())
        throw Error(ErrorCode::not_found, "unknown fixed model plan");
    proto::ModelPlan value;
    if (!value.ParseFromString(read.blob(1)))
        throw std::invalid_argument("invalid stored model plan");
    protocol::validate_model_plan(value);
    if (value.id() != id || value.owner().SerializeAsString() != owner_.SerializeAsString()
        || value.source().result().id() != read.text(0))
        throw Error(ErrorCode::conflict, "model plan owner or index mismatch");
    return value;
}

std::vector<proto::ModelDraft> Models::drafts(const std::string& after, unsigned limit) const
{
    page(after, limit);
    std::vector<proto::ModelDraft> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement read(
        *database_,
        "SELECT id FROM research_model_drafts WHERE id>? ORDER BY id LIMIT ?");
    read.bind(1, after).bind(2, limit);
    while (read.step())
        result.push_back(*draft(read.text(0)));
    return result;
}

std::vector<proto::ModelPlan> Models::plans(const std::string& after, unsigned limit) const
{
    page(after, limit);
    std::vector<proto::ModelPlan> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement read(
        *database_,
        "SELECT id FROM research_model_plans WHERE id>? ORDER BY id LIMIT ?");
    read.bind(1, after).bind(2, limit);
    while (read.step())
        result.push_back(plan(read.text(0)));
    return result;
}

std::int64_t Models::model_published_at(
    const resource::proto::Object& model,
    const resource::proto::Object& selected_plan,
    bool active) const
{
    const bool time = model.kind() == "model_time_validation_result";
    protocol::validate_strategy_object(
        model,
        time ? "model_time_validation_result" : "model_training_result");
    if (model.owner().SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "prediction mapping model owner mismatch");
    Statement read(
        *database_,
        "SELECT CASE WHEN length(record)<=65536 THEN record END FROM tasks WHERE id=?");
    read.bind(1, model.id());
    task::proto::StoredTaskRecord stored;
    if (!read.step() || !stored.ParseFromString(read.blob(0)))
        throw Error(ErrorCode::conflict, "prediction mapping original model is missing");
    protocol::validate_message(stored);
    const auto& task = stored.task();
    const auto temporal = time ? time_plan(selected_plan.id()) : proto::ModelTimePlan{};
    const auto fixed = time ? temporal.recipe() : plan(selected_plan.id());
    const auto expected_plan =
        time ? protocol::model_time_plan_object(temporal) : protocol::model_plan_object(fixed);
    const auto& task_plan = time ? task.model_time_plan() : task.model_plan();
    if (task.kind() != (time ? task::proto::MODEL_TIME_VALIDATION : task::proto::MODEL_TRAINING)
        || task.state() != task::proto::SUCCEEDED || task.id() != model.id()
        || task.result_digest() != model.version()
        || task_plan.SerializeAsString() != selected_plan.SerializeAsString()
        || expected_plan.SerializeAsString() != selected_plan.SerializeAsString()
        || task.program_artifact() != fixed.program_artifact() || (active && task.has_retirement()))
        throw Error(
            ErrorCode::conflict,
            "prediction mapping requires its original successful model");
    Statement event(
        *database_,
        "SELECT json_extract(body,'$.updated_at_ms') FROM task_events WHERE task_id=? AND json_extract(body,'$.type')='state' AND json_extract(body,'$.state')=? AND json_extract(body,'$.digest')=? ORDER BY sequence DESC LIMIT 1");
    event.bind(1, model.id())
        .bind(2, static_cast<int>(task::proto::SUCCEEDED))
        .bind(3, model.version());
    if (!event.step() || event.integer(0) <= 0)
        throw Error(ErrorCode::conflict, "model successful publication evidence is missing");
    return event.integer(0);
}

void Models::verify_mapping(const proto::ModelMapping& value, bool active) const
{
    protocol::validate_model_mapping(value);
    const bool time = value.specification().model().kind() == "model_time_validation_result";
    const auto temporal = time ? time_plan(value.plan().id()) : proto::ModelTimePlan{};
    const auto fixed = time ? temporal.recipe() : plan(value.plan().id());
    if (time) {
        if (value.folds_size() != temporal.specification().folds_size())
            throw Error(ErrorCode::conflict, "prediction mapping original fold scope mismatch");
        for (int i = 0; i < value.folds_size(); ++i)
            if (value.folds(i).SerializeAsString()
                != temporal.specification().folds(i).SerializeAsString())
                throw Error(ErrorCode::conflict, "prediction mapping original fold scope mismatch");
    }
    if (value.owner().SerializeAsString() != owner_.SerializeAsString()
        || value.source().SerializeAsString() != fixed.source().SerializeAsString()
        || value.specification().valid_until_ms()
            > (time ? temporal.specification().valid_until_ms()
                    : fixed.specification().valid_until_ms())
        || model_published_at(value.specification().model(), value.plan(), active)
            != value.model_published_at_ms())
        throw Error(ErrorCode::conflict, "prediction mapping original model contract changed");
}

proto::ModelMapping Models::capture_mapping(
    const proto::ModelMappingSave& request,
    const task::proto::TaskResponse& verified) const
{
    protocol::validate_model_mapping_specification(request.specification());
    const auto& task = verified.result_task();
    const bool time = request.specification().model().kind() == "model_time_validation_result";
    if (!verified.has_result_task() || task.id() != request.specification().model().id()
        || task.result_digest() != request.specification().model().version()
        || (time ? (!verified.has_model_time() || !task.has_model_time())
                 : (!verified.has_model_training() || !task.has_model_training())))
        throw Error(
            ErrorCode::conflict,
            "prediction mapping requires verified original model bytes");
    const auto bytes = time ? verified.model_time().SerializeAsString()
                            : verified.model_training().SerializeAsString();
    if (task.result_digest() != sha256_bytes(bytes))
        throw Error(
            ErrorCode::conflict,
            "prediction mapping requires verified original model bytes");
    proto::ModelMapping value;
    value.set_id(request.mapping_id());
    *value.mutable_owner() = owner_;
    *value.mutable_specification() = request.specification();
    if (time) {
        protocol::validate_model_time_result(task.model_time(), verified.model_time());
        const auto& fixed = task.model_time().plan();
        if (time_plan(fixed.id()).SerializeAsString() != fixed.SerializeAsString())
            throw Error(ErrorCode::conflict, "prediction mapping model plan differs from original");
        *value.mutable_plan() = protocol::model_time_plan_object(fixed);
        *value.mutable_source() = fixed.recipe().source();
        *value.mutable_folds() = fixed.specification().folds();
    } else {
        protocol::validate_model_training_result(task.model_training(), verified.model_training());
        const auto& fixed = task.model_training().plan();
        if (plan(fixed.id()).SerializeAsString() != fixed.SerializeAsString())
            throw Error(ErrorCode::conflict, "prediction mapping model plan differs from original");
        *value.mutable_plan() = protocol::model_plan_object(fixed);
        *value.mutable_source() = fixed.source();
    }
    value.set_model_published_at_ms(
        model_published_at(request.specification().model(), value.plan(), true));
    value.set_recorded_at_ms(clock_->utc_now() / 1000000);
    value.set_mode(proto::ModelSpecification::POST_HOC_DIAGNOSTIC);
    value.set_digest(protocol::model_mapping_digest(value));
    verify_mapping(value, true);
    return value;
}

proto::ModelMapping Models::mapping(const std::string& id) const
{
    validate_id(id);
    if (!inspect(*database_, owner_))
        throw Error(ErrorCode::not_found, "unknown fixed prediction mapping");
    Statement read(
        *database_,
        "SELECT source_id,CASE WHEN length(body)<=16384 THEN body END FROM research_model_mappings WHERE id=?");
    read.bind(1, id);
    proto::ModelMapping value;
    if (!read.step())
        throw Error(ErrorCode::not_found, "unknown fixed prediction mapping");
    if (!value.ParseFromString(read.blob(1)) || value.id() != id
        || value.specification().model().id() != read.text(0))
        throw std::invalid_argument("invalid stored prediction mapping");
    verify_mapping(value, false);
    return value;
}

std::vector<proto::ModelMapping> Models::mappings(const std::string& after, unsigned limit) const
{
    page(after, limit);
    std::vector<proto::ModelMapping> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement read(
        *database_,
        "SELECT id FROM research_model_mappings WHERE id>? ORDER BY id LIMIT ?");
    read.bind(1, after).bind(2, limit);
    while (read.step())
        result.push_back(mapping(read.text(0)));
    return result;
}

void Models::verify_time_plan(const proto::ModelTimePlan& value, bool active) const
{
    protocol::validate_model_time_plan(value);
    if (value.owner().SerializeAsString() != owner_.SerializeAsString()
        || plan(value.recipe().id()).SerializeAsString() != value.recipe().SerializeAsString())
        throw Error(
            ErrorCode::conflict,
            "model time plan differs from its original retained recipe");
    verify_source(value.recipe(), active);
}

proto::ModelTimePlan Models::capture_time(
    const proto::ModelTimeSave& request,
    const task::proto::TaskResponse& verified) const
{
    const auto recipe = plan(request.specification().recipe().id());
    verify_source(recipe, true);
    return capture_model_time_plan(owner_, request, recipe, verified, clock_->utc_now() / 1000000);
}

proto::ModelTimePlan Models::time_plan(const std::string& id) const
{
    validate_id(id);
    if (!inspect(*database_, owner_))
        throw Error(ErrorCode::not_found, "unknown fixed model time plan");
    Statement read(
        *database_,
        "SELECT source_id,CASE WHEN length(body)<=65536 THEN body END FROM research_model_time_plans WHERE id=?");
    read.bind(1, id);
    proto::ModelTimePlan value;
    if (!read.step())
        throw Error(ErrorCode::not_found, "unknown fixed model time plan");
    if (!value.ParseFromString(read.blob(1)) || value.id() != id
        || value.recipe().source().result().id() != read.text(0))
        throw std::invalid_argument("invalid retained model time plan");
    verify_time_plan(value, false);
    return value;
}

std::vector<proto::ModelTimePlan> Models::time_plans(const std::string& after, unsigned limit) const
{
    page(after, limit);
    std::vector<proto::ModelTimePlan> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement read(
        *database_,
        "SELECT id FROM research_model_time_plans WHERE id>? ORDER BY id LIMIT ?");
    read.bind(1, after).bind(2, limit);
    while (read.step())
        result.push_back(time_plan(read.text(0)));
    return result;
}

proto::ModelTimePlan Models::time_admission(
    const resource::proto::Object& object,
    const std::string& loaded_program) const
{
    protocol::validate_strategy_object(object, "model_time_plan");
    const auto value = time_plan(object.id());
    if (protocol::model_time_plan_object(value).SerializeAsString() != object.SerializeAsString()
        || value.recipe().program_artifact() != loaded_program)
        throw Error(
            ErrorCode::conflict,
            "model time training requires its exact fixed plan and program");
    verify_time_plan(value, true);
    if (clock_->utc_now() / 1000000 >= value.specification().valid_until_ms())
        throw Error(ErrorCode::conflict, "fixed model time plan has expired");
    return value;
}

std::optional<proto::ModelReply> Models::receipt(const proto::ModelMutation& command) const
{
    protocol::validate_model_mutation(command);
    if (!inspect(*database_, owner_))
        return {};
    Statement read(
        *database_,
        "SELECT CASE WHEN length(request)<=32768 THEN request END,CASE WHEN length(response)<=65536 THEN response END FROM research_model_commands WHERE id=?");
    read.bind(1, command.id());
    if (!read.step())
        return {};
    if (read.blob(0) != command.SerializeAsString())
        throw Error(ErrorCode::conflict, "model request id reused with different content");
    proto::ModelReply response;
    if (!response.ParseFromString(read.blob(1)))
        throw std::invalid_argument("invalid stored model receipt");
    protocol::validate_model_mutation_reply(command, response, owner_);
    return response;
}

void Models::verify_source(const proto::ModelPlan& value, bool active) const
{
    const auto& source = value.source();
    Statement read(
        *database_,
        "SELECT CASE WHEN length(record)<=65536 THEN record END FROM tasks WHERE id=?");
    read.bind(1, source.result().id());
    task::proto::StoredTaskRecord record;
    if (!read.step() || !record.ParseFromString(read.blob(0)))
        throw Error(ErrorCode::conflict, "model original source record missing");
    protocol::validate_message(record);
    const auto& task = record.task();
    if (task.id() != source.result().id() || task.kind() != task::proto::FACTOR
        || task.state() != task::proto::SUCCEEDED
        || task.result_digest() != source.result().version()
        || record.input_digest() != source.input_sha256()
        || record.input_bytes() != source.input_bytes() || task.attempt() != source.attempt()
        || task.program_artifact() != source.program_artifact()
        || (active && task.has_retirement()))
        throw Error(
            ErrorCode::conflict,
            "model original source identity is changed or unavailable");
}

proto::ModelReply Models::apply(
    const proto::ModelMutation& command,
    const proto::ModelPlan* captured,
    const proto::ModelMapping* captured_mapping,
    const proto::ModelTimePlan* captured_time)
{
    if (!writer_)
        throw Error(ErrorCode::permission_denied, "research model access is read only");
    protocol::validate_model_mutation(command);
    Database::Transaction tx(*database_);
    if (const auto prior = receipt(command))
        return *prior;
    proto::ModelReply response;
    if (command.has_save()) {
        const auto& request = command.save();
        const auto previous = draft(request.draft_id());
        if ((previous ? previous->digest() : "") != request.expected_digest())
            throw Error(ErrorCode::conflict, "model draft changed; read original before saving");
        auto& value = *response.mutable_draft();
        value.set_id(request.draft_id());
        *value.mutable_owner() = owner_;
        *value.mutable_specification() = request.specification();
        value.set_updated_at_ms(clock_->utc_now() / 1000000);
        value.set_digest(protocol::model_draft_digest(value));
        protocol::validate_model_draft(value);
        if (value.specification().has_source()
            && value.specification().source().owner().SerializeAsString()
                != owner_.SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "model draft source must belong to this Research service");
        Statement write(
            *database_,
            "INSERT INTO research_model_drafts VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET source_id=excluded.source_id,body=excluded.body");
        write.bind(1, value.id())
            .bind(2, value.specification().source().id())
            .bind_blob(3, value.SerializeAsString())
            .step();
    } else if (command.has_time_validation()) {
        if (!captured_time)
            throw std::invalid_argument("verified model time plan required");
        verify_time_plan(*captured_time, true);
        if (captured_time->id() != command.time_validation().plan_id()
            || captured_time->specification().SerializeAsString()
                != command.time_validation().specification().SerializeAsString()
            || clock_->utc_now() / 1000000 >= captured_time->specification().valid_until_ms())
            throw Error(
                ErrorCode::conflict,
                "model time plan differs from current explicit intent");
        Statement exists(*database_, "SELECT 1 FROM research_model_time_plans WHERE id=?");
        exists.bind(1, captured_time->id());
        if (exists.step())
            throw Error(ErrorCode::conflict, "model time plan identity already exists");
        Statement write(*database_, "INSERT INTO research_model_time_plans VALUES(?,?,?)");
        write.bind(1, captured_time->id())
            .bind(2, captured_time->recipe().source().result().id())
            .bind_blob(3, captured_time->SerializeAsString())
            .step();
        *response.mutable_time_plan() = *captured_time;
    } else if (command.has_mapping()) {
        if (!captured_mapping)
            throw std::invalid_argument("verified prediction mapping required");
        verify_mapping(*captured_mapping, true);
        if (captured_mapping->id() != command.mapping().mapping_id()
            || captured_mapping->specification().SerializeAsString()
                != command.mapping().specification().SerializeAsString()
            || clock_->utc_now() / 1000000 >= captured_mapping->specification().valid_until_ms())
            throw Error(
                ErrorCode::conflict,
                "prediction mapping does not match current explicit intent");
        Statement exists(*database_, "SELECT 1 FROM research_model_mappings WHERE id=?");
        exists.bind(1, captured_mapping->id());
        if (exists.step())
            throw Error(ErrorCode::conflict, "prediction mapping identity already exists");
        Statement write(*database_, "INSERT INTO research_model_mappings VALUES(?,?,?)");
        write.bind(1, captured_mapping->id())
            .bind(2, captured_mapping->specification().model().id())
            .bind_blob(3, captured_mapping->SerializeAsString())
            .step();
        *response.mutable_mapping() = *captured_mapping;
    } else {
        const auto& request = command.fix();
        const auto current = draft(request.draft_id());
        if (!current || current->digest() != request.expected_digest())
            throw Error(ErrorCode::conflict, "model fixing requires the original current draft");
        if (!captured)
            throw std::invalid_argument("verified model source and plan required");
        protocol::validate_model_plan(*captured);
        if (captured->id() != request.plan_id() || captured->draft_id() != current->id()
            || captured->draft_digest() != current->digest()
            || captured->owner().SerializeAsString() != owner_.SerializeAsString()
            || captured->specification().SerializeAsString()
                != current->specification().SerializeAsString())
            throw Error(ErrorCode::conflict, "captured model plan does not match original draft");
        verify_source(*captured, true);
        Statement exists(*database_, "SELECT 1 FROM research_model_plans WHERE id=?");
        exists.bind(1, request.plan_id());
        if (exists.step())
            throw Error(ErrorCode::conflict, "model plan identity already exists");
        Statement write(*database_, "INSERT INTO research_model_plans VALUES(?,?,?)");
        write.bind(1, captured->id())
            .bind(2, captured->source().result().id())
            .bind_blob(3, captured->SerializeAsString())
            .step();
        *response.mutable_plan() = *captured;
    }
    protocol::validate_model_mutation_reply(command, response, owner_);
    Statement write(*database_, "INSERT INTO research_model_commands VALUES(?,?,?)");
    write.bind(1, command.id())
        .bind_blob(2, command.SerializeAsString())
        .bind_blob(3, response.SerializeAsString())
        .step();
    tx.commit();
    return response;
}

proto::ModelPlan
Models::admission(const resource::proto::Object& object, const std::string& program) const
{
    protocol::validate_strategy_object(object, "model_plan");
    const auto value = plan(object.id());
    if (object.SerializeAsString() != protocol::model_plan_object(value).SerializeAsString()
        || program != value.program_artifact())
        throw Error(
            ErrorCode::conflict,
            "training requires its fixed model plan and execution program");
    verify_source(value, true);
    if (clock_->utc_now() / 1000000 >= value.specification().valid_until_ms())
        throw Error(ErrorCode::conflict, "fixed model plan has expired");
    return value;
}

std::vector<std::string> Models::source_consumers(const std::string& id) const
{
    validate_id(id);
    std::vector<std::string> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement drafts(
        *database_,
        "SELECT id FROM research_model_drafts WHERE source_id=? ORDER BY id");
    drafts.bind(1, id);
    while (drafts.step()) {
        const auto value = draft(drafts.text(0));
        result.push_back("model-draft:" + value->id());
    }
    Statement plans(
        *database_,
        "SELECT id FROM research_model_plans WHERE source_id=? ORDER BY id");
    plans.bind(1, id);
    while (plans.step()) {
        const auto value = plan(plans.text(0));
        result.push_back("model-plan:" + value.id());
    }
    Statement mappings(
        *database_,
        "SELECT id FROM research_model_mappings WHERE source_id=? ORDER BY id");
    mappings.bind(1, id);
    while (mappings.step())
        result.push_back("model-mapping:" + mapping(mappings.text(0)).id());
    Statement time_plans(
        *database_,
        "SELECT id FROM research_model_time_plans WHERE source_id=? ORDER BY id");
    time_plans.bind(1, id);
    while (time_plans.step())
        result.push_back("model-time-plan:" + time_plan(time_plans.text(0)).id());
    return result;
}

resource::proto::Catalogue Models::resources(const resource::proto::CatalogueQuery& query) const
{
    const auto limit = protocol::catalogue_limit(query);
    if (query.kind() != "model_draft" && query.kind() != "model_plan"
        && query.kind() != "model_mapping" && query.kind() != "model_time_plan")
        throw Error(ErrorCode::invalid_request, "resource kind is not implemented by Research");
    resource::proto::Catalogue result;
    *result.mutable_owner() = owner_;
    result.set_kind(query.kind());
    if (query.kind() == "model_draft") {
        const auto rows = drafts(query.after(), limit);
        for (const auto& value : rows) {
            auto& row = *result.add_items();
            *row.mutable_object() = protocol::model_draft_object(value);
            row.set_name(value.specification().name());
            row.set_state(resource::proto::CatalogueEntry::REGISTERED);
            row.set_logical_bytes(value.ByteSizeLong());
            row.set_relative_location("tasks.sqlite");
        }
        if (rows.size() == limit && !drafts(rows.back().id(), 1).empty())
            result.set_next_after(rows.back().id());
    } else if (query.kind() == "model_plan") {
        const auto rows = plans(query.after(), limit);
        for (const auto& value : rows) {
            auto& row = *result.add_items();
            *row.mutable_object() = protocol::model_plan_object(value);
            row.set_name(value.specification().name());
            row.set_state(resource::proto::CatalogueEntry::SEALED);
            row.set_logical_bytes(value.ByteSizeLong());
            row.set_relative_location("tasks.sqlite");
        }
        if (rows.size() == limit && !plans(rows.back().id(), 1).empty())
            result.set_next_after(rows.back().id());
    } else if (query.kind() == "model_mapping") {
        const auto rows = mappings(query.after(), limit);
        for (const auto& value : rows) {
            auto& row = *result.add_items();
            *row.mutable_object() = protocol::model_mapping_object(value);
            row.set_name(value.specification().name());
            row.set_state(resource::proto::CatalogueEntry::SEALED);
            row.set_logical_bytes(value.ByteSizeLong());
            row.set_relative_location("tasks.sqlite");
        }
        if (rows.size() == limit && !mappings(rows.back().id(), 1).empty())
            result.set_next_after(rows.back().id());
    } else {
        const auto rows = time_plans(query.after(), limit);
        for (const auto& value : rows) {
            auto& row = *result.add_items();
            *row.mutable_object() = protocol::model_time_plan_object(value);
            row.set_name(value.specification().name());
            row.set_state(resource::proto::CatalogueEntry::SEALED);
            row.set_logical_bytes(value.ByteSizeLong());
            row.set_relative_location("tasks.sqlite");
        }
        if (rows.size() == limit && !time_plans(rows.back().id(), 1).empty())
            result.set_next_after(rows.back().id());
    }
    return result;
}

void Models::inspect_backup(
    const std::filesystem::path& root,
    const resource::proto::Owner& expected,
    std::span<const proto::ModelTrainingInput> required,
    const std::function<void(const proto::ModelDraft&)>& draft_visitor,
    const std::function<void(const proto::ModelPlan&)>& plan_visitor,
    std::stop_token stop,
    const std::function<void(const proto::ModelMapping&)>& mapping_visitor,
    const std::function<void(const proto::ModelTimePlan&)>& time_visitor)
{
    owner(expected);
    FileLock ownership(root, "manager.lock", FileLock::Access::shared_existing);
    Database database(root / "tasks.sqlite", Database::Access::cold_read_only);
    Models view(
        database,
        expected,
        Database::Access::cold_read_only,
        std::make_shared<SystemClock>());
    const auto check = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "Research model cold inspection interrupted");
    };
    for (const auto& input : required) {
        check();
        protocol::validate_model_training_input(input);
        if (view.plan(input.plan().id()).SerializeAsString() != input.plan().SerializeAsString())
            throw Error(ErrorCode::conflict, "model task differs from its retained immutable plan");
    }
    std::string cursor;
    for (;;) {
        check();
        const auto rows = view.drafts(cursor, 200);
        if (rows.empty())
            break;
        for (const auto& draft : rows) {
            check();
            draft_visitor(draft);
        }
        cursor = rows.back().id();
    }
    cursor.clear();
    for (;;) {
        check();
        const auto rows = view.plans(cursor, 200);
        if (rows.empty())
            break;
        for (const auto& plan : rows) {
            check();
            view.verify_source(plan, false);
            if (!view.draft(plan.draft_id()))
                throw std::invalid_argument("fixed model plan draft is missing");
            plan_visitor(plan);
        }
        cursor = rows.back().id();
    }
    cursor.clear();
    for (;;) {
        check();
        const auto rows = view.mappings(cursor, 200);
        if (rows.empty())
            break;
        for (const auto& mapping : rows) {
            check();
            if (mapping_visitor)
                mapping_visitor(mapping);
        }
        cursor = rows.back().id();
    }
    cursor.clear();
    for (;;) {
        check();
        const auto rows = view.time_plans(cursor, 200);
        if (rows.empty())
            break;
        for (const auto& value : rows) {
            check();
            if (time_visitor)
                time_visitor(value);
        }
        cursor = rows.back().id();
    }
    if (!inspect(database, expected))
        return;
    Statement commands(
        database,
        "SELECT id,CASE WHEN length(request)<=32768 THEN request END FROM research_model_commands ORDER BY id");
    while (commands.step()) {
        check();
        proto::ModelMutation request;
        if (!request.ParseFromString(commands.blob(1)) || request.id() != commands.text(0))
            throw std::invalid_argument("invalid retained model command");
        const auto receipt = view.receipt(request);
        if (!receipt)
            throw std::invalid_argument("retained model command receipt is missing");
        if (receipt->has_plan()) {
            if (view.plan(receipt->plan().id()).SerializeAsString()
                != receipt->plan().SerializeAsString())
                throw std::invalid_argument("model receipt differs from its immutable plan");
        } else if (receipt->has_time_plan()) {
            if (view.time_plan(receipt->time_plan().id()).SerializeAsString()
                != receipt->time_plan().SerializeAsString())
                throw std::invalid_argument("model time receipt differs from immutable plan");
        } else if (receipt->has_mapping()) {
            if (view.mapping(receipt->mapping().id()).SerializeAsString()
                != receipt->mapping().SerializeAsString())
                throw std::invalid_argument(
                    "prediction mapping receipt differs from immutable object");
        } else if (!view.draft(receipt->draft().id()))
            throw std::invalid_argument("model receipt draft is missing");
        // Draft save receipts intentionally keep the accepted old revision after later edits.
    }
}

} // namespace asterion::research
