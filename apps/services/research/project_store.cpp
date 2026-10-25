#include "project_store.hpp"
#include "task_store.hpp"
#include <asterion/protocol/research_project.hpp>
#include <asterion/protocol/resource.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/protocol/trading.hpp>
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

void definition(const proto::ProjectDefinition& value)
{
    protocol::validate_message(value);
    if (blank(value.title()))
        throw std::invalid_argument("research project title required");
    if (value.title().size() > 256 || value.ByteSizeLong() > 65536)
        throw std::invalid_argument("research project definition exceeds limit");
}

std::string draft_digest(proto::ProjectDraft value)
{
    value.clear_digest();
    value.clear_updated_at_ms();
    return sha256_bytes(value.SerializeAsString());
}

std::string plan_digest(proto::HypothesisPlan value)
{
    value.clear_definition_digest();
    return sha256_bytes(value.SerializeAsString());
}

void owner(const resource::proto::Owner& value)
{
    protocol::validate_message(value);
    validate_id(value.node_id());
    validate_id(value.service_id());
    if (value.domain() != "research")
        throw std::invalid_argument("research project owner required");
}

void validate(const proto::ProjectDraft& value, const resource::proto::Owner& expected)
{
    protocol::validate_message(value);
    validate_id(value.id());
    definition(value.definition());
    if (value.owner().SerializeAsString() != expected.SerializeAsString()
        || value.digest() != draft_digest(value) || value.updated_at_ms() < 0)
        throw std::invalid_argument("invalid research project record");
}

void validate(const proto::HypothesisPlan& value, const resource::proto::Owner& expected)
{
    protocol::validate_message(value);
    validate_id(value.id());
    validate_id(value.project_id());
    definition(value.definition());
    if (value.ByteSizeLong() > 131072)
        throw std::invalid_argument("research project definition exceeds limit");
    if (!value.parent_plan_id().empty())
        validate_id(value.parent_plan_id());
    if (value.owner().SerializeAsString() != expected.SerializeAsString()
        || value.definition_digest() != plan_digest(value) || value.recorded_at_ms() < 0
        || blank(value.definition().question()) || blank(value.definition().hypothesis())
        || value.exploratory()
            != (blank(value.definition().support()) || blank(value.definition().refutation()))
        || (!value.parent_plan_id().empty() && blank(value.revision_reason())))
        throw std::invalid_argument("invalid research hypothesis plan");
}

std::string conclusion_digest(proto::ProjectConclusion value)
{
    value.clear_digest();
    return sha256_bytes(value.SerializeAsString());
}

void validate(const proto::ProjectConclusion& value, const resource::proto::Owner& expected)
{
    protocol::validate_message(value);
    const auto& d = value.declaration();
    validate_id(d.id());
    validate_id(d.result().id());
    protocol::validate_project_binding(d.plan());
    if (!d.previous_id().empty())
        validate_id(d.previous_id());
    const auto hash = [](const std::string& s) {
        return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
    };
    if (d.plan().owner().SerializeAsString() != expected.SerializeAsString()
        || d.result().owner().SerializeAsString() != expected.SerializeAsString()
        || (d.result().kind() != "factor_result" && d.result().kind() != "sample_result"
            && d.result().kind() != "statistics_result" && d.result().kind() != "backtest_result"
            && d.result().kind() != "performance_evaluation_result")
        || !hash(d.result().version()) || !hash(value.input_sha256())
        || !hash(value.program_artifact()) || d.judgment() < proto::SUPPORTED
        || d.judgment() > proto::INSUFFICIENT || blank(d.reason()) || value.ByteSizeLong() > 131072
        || (!d.previous_id().empty() && blank(d.revision_reason())) || value.recorded_at_ms() < 0
        || value.digest() != conclusion_digest(value))
        throw std::invalid_argument("invalid research conclusion");
}

task::proto::StoredTaskRecord conclusion_source(Database& db, const resource::proto::Object& result)
{
    Statement read(
        db,
        "SELECT CASE WHEN length(record)<=65536 THEN record END,state,attempt FROM tasks WHERE id=?");
    read.bind(1, result.id());
    task::proto::StoredTaskRecord record;
    if (!read.step() || !record.ParseFromString(read.blob(0)))
        throw Error(ErrorCode::not_found, "unknown research conclusion result");
    protocol::validate_message(record);
    const auto& task = record.task();
    const bool kind_matches =
        (task.kind() == task::proto::FACTOR && result.kind() == "factor_result")
        || (task.kind() == task::proto::SAMPLE && result.kind() == "sample_result")
        || (task.kind() == task::proto::STATISTICS && result.kind() == "statistics_result")
        || (task.kind() == task::proto::BACKTEST && result.kind() == "backtest_result")
        || (task.kind() == task::proto::PERFORMANCE_EVALUATION
            && result.kind() == "performance_evaluation_result");
    if (task.id() != result.id() || !kind_matches || task.state() != task::proto::SUCCEEDED
        || task.state() != read.integer(1) || !task.attempt() || task.attempt() != read.integer(2)
        || task.result_digest() != result.version())
        throw Error(ErrorCode::conflict, "research conclusion result mismatch");
    return record;
}

void validate_receipt(
    const proto::ProjectMutation& request,
    const proto::ProjectReply& reply,
    const resource::proto::Owner& expected)
{
    protocol::validate_message(request);
    protocol::validate_message(reply);
    validate_id(request.id());
    if (request.ByteSizeLong() > 131072 || reply.ByteSizeLong() > 131072 || reply.drafts_size()
        || reply.plans_size() || reply.conclusions_size() || !reply.next_after().empty())
        throw std::invalid_argument("invalid research project receipt");
    if (request.has_save()) {
        if (!reply.has_draft() || reply.has_plan() || reply.has_conclusion()
            || reply.draft().id() != request.save().project_id()
            || reply.draft().definition().SerializeAsString()
                != request.save().definition().SerializeAsString())
            throw std::invalid_argument("invalid research project receipt");
        validate(reply.draft(), expected);
    } else if (request.has_fix()) {
        if (!reply.has_plan() || reply.has_draft() || reply.has_conclusion())
            throw std::invalid_argument("invalid research project receipt");
        const auto& fixed = reply.plan();
        const auto& fix = request.fix();
        validate(fixed, expected);
        proto::ProjectDraft source;
        source.set_id(fixed.project_id());
        *source.mutable_owner() = expected;
        *source.mutable_definition() = fixed.definition();
        if (fixed.id() != fix.plan_id() || fixed.project_id() != fix.project_id()
            || fixed.parent_plan_id() != fix.parent_plan_id()
            || fixed.revision_reason() != fix.revision_reason()
            || draft_digest(source) != fix.expected_draft_digest())
            throw std::invalid_argument("invalid research project receipt");
    } else if (request.has_conclude()) {
        if (!reply.has_conclusion() || reply.has_draft() || reply.has_plan()
            || reply.conclusion().declaration().SerializeAsString()
                != request.conclude().SerializeAsString())
            throw std::invalid_argument("invalid research project receipt");
        validate(reply.conclusion(), expected);
    } else
        throw std::invalid_argument("invalid research project receipt");
}

bool inspect(Database& db, const resource::proto::Owner& expected)
{
    // TaskStore owns this same file and its root lock; never create a standalone
    // empty database under an arbitrary path passed by a client.
    {
        Statement task(db, "SELECT id FROM tasks LIMIT 1");
        (void)task.step();
    }
    Statement tables(
        db,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('research_project_owner','research_project_drafts','research_hypothesis_plans','research_project_commands','research_project_conclusions')");
    (void)tables.step();
    const auto count = tables.integer(0);
    if (!count)
        return false;
    if (count != 5)
        throw std::invalid_argument("incomplete research project schema");
    Statement owned(
        db,
        "SELECT CASE WHEN length(body)<=4096 THEN body END FROM research_project_owner WHERE singleton=1");
    resource::proto::Owner actual;
    if (!owned.step() || !actual.ParseFromString(owned.blob(0)))
        throw std::invalid_argument("invalid research project owner");
    owner(actual);
    if (actual.SerializeAsString() != expected.SerializeAsString())
        throw Error(ErrorCode::conflict, "research project owner mismatch");
    return true;
}

void page(const std::string& after, unsigned limit)
{
    if (!after.empty())
        validate_id(after);
    if (!limit || limit > 200)
        throw std::invalid_argument("invalid research project page size");
}

} // namespace

Projects::Projects(
    tasks::Store& store,
    resource::proto::Owner expected,
    Database::Access access,
    std::shared_ptr<const Clock> clock)
    : Projects(
          store.research_metadata(expected.service_id(), access == Database::Access::writer),
          expected,
          access,
          std::move(clock))
{}

Projects::Projects(
    Database& database,
    resource::proto::Owner expected,
    Database::Access access,
    std::shared_ptr<const Clock> clock)
    : owner_(std::move(expected)),
      clock_(std::move(clock)),
      database_(&database),
      writer_(access == Database::Access::writer)
{
    owner(owner_);
    initialized_ = inspect(*database_, owner_);
    if (writer_ && !initialized_) {
        Database::Transaction transaction(*database_);
        database_->execute(
            "CREATE TABLE research_project_owner(singleton INTEGER PRIMARY KEY CHECK(singleton=1), body BLOB NOT NULL) STRICT");
        database_->execute(
            "CREATE TABLE research_project_drafts(id TEXT PRIMARY KEY, body BLOB NOT NULL) STRICT");
        database_->execute(
            "CREATE TABLE research_hypothesis_plans(id TEXT PRIMARY KEY, project_id TEXT NOT NULL REFERENCES research_project_drafts(id), body BLOB NOT NULL) STRICT");
        database_->execute(
            "CREATE INDEX research_hypothesis_project ON research_hypothesis_plans(project_id,id)");
        database_->execute(
            "CREATE TABLE research_project_commands(id TEXT PRIMARY KEY, request BLOB NOT NULL, response BLOB NOT NULL) STRICT");
        database_->execute(
            "CREATE TABLE research_project_conclusions(id TEXT PRIMARY KEY,project_id TEXT NOT NULL REFERENCES research_project_drafts(id),body BLOB NOT NULL) STRICT");
        database_->execute(
            "CREATE INDEX research_conclusions_project ON research_project_conclusions(project_id,id)");
        Statement insert(*database_, "INSERT INTO research_project_owner VALUES(1,?)");
        insert.bind_blob(1, owner_.SerializeAsString()).step();
        transaction.commit();
        initialized_ = true;
    }
}

Projects::~Projects() = default;

std::optional<proto::ProjectDraft> Projects::draft(const std::string& id) const
{
    validate_id(id);
    if (!inspect(*database_, owner_))
        return {};
    Statement read(
        *database_,
        "SELECT CASE WHEN length(body)<=131072 THEN body END FROM research_project_drafts WHERE id=?");
    read.bind(1, id);
    if (!read.step())
        return {};
    proto::ProjectDraft result;
    if (!result.ParseFromString(read.blob(0)) || result.id() != id)
        throw std::invalid_argument("invalid research project record");
    validate(result, owner_);
    return result;
}

proto::HypothesisPlan Projects::plan(const std::string& id) const
{
    validate_id(id);
    if (!inspect(*database_, owner_))
        throw Error(ErrorCode::not_found, "unknown research hypothesis plan");
    Statement read(
        *database_,
        "SELECT project_id, CASE WHEN length(body)<=131072 THEN body END FROM research_hypothesis_plans WHERE id=?");
    read.bind(1, id);
    if (!read.step())
        throw Error(ErrorCode::not_found, "unknown research hypothesis plan");
    proto::HypothesisPlan result;
    if (!result.ParseFromString(read.blob(1)) || result.id() != id
        || result.project_id() != read.text(0))
        throw std::invalid_argument("invalid research hypothesis plan");
    validate(result, owner_);
    return result;
}

std::vector<proto::ProjectDraft> Projects::drafts(const std::string& after, unsigned limit) const
{
    page(after, limit);
    std::vector<proto::ProjectDraft> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement read(
        *database_,
        "SELECT id FROM research_project_drafts WHERE id>? ORDER BY id LIMIT ?");
    read.bind(1, after).bind(2, static_cast<std::int64_t>(limit));
    while (read.step())
        result.push_back(*draft(read.text(0)));
    return result;
}

std::vector<proto::HypothesisPlan>
Projects::plans(const std::string& project, const std::string& after, unsigned limit) const
{
    if (!project.empty())
        validate_id(project);
    page(after, limit);
    std::vector<proto::HypothesisPlan> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement read(
        *database_,
        project.empty()
            ? "SELECT id FROM research_hypothesis_plans WHERE id>? ORDER BY id LIMIT ?"
            : "SELECT id FROM research_hypothesis_plans WHERE project_id=? AND id>? ORDER BY id LIMIT ?");
    if (project.empty())
        read.bind(1, after).bind(2, static_cast<std::int64_t>(limit));
    else
        read.bind(1, project).bind(2, after).bind(3, static_cast<std::int64_t>(limit));
    while (read.step())
        result.push_back(plan(read.text(0)));
    return result;
}

proto::ProjectConclusion Projects::conclusion(const std::string& id) const
{
    validate_id(id);
    if (!inspect(*database_, owner_))
        throw Error(ErrorCode::not_found, "unknown research conclusion");
    Statement read(
        *database_,
        "SELECT project_id,CASE WHEN length(body)<=131072 THEN body END FROM research_project_conclusions WHERE id=?");
    read.bind(1, id);
    proto::ProjectConclusion result;
    if (!read.step())
        throw Error(ErrorCode::not_found, "unknown research conclusion");
    if (!result.ParseFromString(read.blob(1)) || result.declaration().id() != id
        || result.declaration().plan().project_id() != read.text(0))
        throw std::invalid_argument("invalid research conclusion");
    validate(result, owner_);
    verify_binding(result.declaration().plan());
    const auto original = conclusion_source(*database_, result.declaration().result());
    if (result.input_sha256() != original.input_digest()
        || result.program_artifact() != original.task().program_artifact()
        || result.retrospective()
            != (original.task().research_plan().SerializeAsString()
                != result.declaration().plan().SerializeAsString()))
        throw Error(ErrorCode::conflict, "research conclusion evidence mismatch");
    return result;
}

std::vector<proto::ProjectConclusion>
Projects::conclusions(const std::string& project, const std::string& after, unsigned limit) const
{
    if (!project.empty())
        validate_id(project);
    page(after, limit);
    std::vector<proto::ProjectConclusion> result;
    if (!inspect(*database_, owner_))
        return result;
    Statement read(
        *database_,
        project.empty()
            ? "SELECT id FROM research_project_conclusions WHERE id>? ORDER BY id LIMIT ?"
            : "SELECT id FROM research_project_conclusions WHERE project_id=? AND id>? ORDER BY id LIMIT ?");
    if (project.empty())
        read.bind(1, after).bind(2, static_cast<std::int64_t>(limit));
    else
        read.bind(1, project).bind(2, after).bind(3, static_cast<std::int64_t>(limit));
    while (read.step())
        result.push_back(conclusion(read.text(0)));
    return result;
}

proto::ProjectReply Projects::apply(const proto::ProjectMutation& request)
{
    if (!writer_)
        throw Error(ErrorCode::permission_denied, "research project access is read only");
    protocol::validate_message(request);
    validate_id(request.id());
    if (request.ByteSizeLong() > 131072
        || (!request.has_save() && !request.has_fix() && !request.has_conclude()))
        throw std::invalid_argument("invalid research project mutation");
    const auto bytes = request.SerializeAsString();
    Database::Transaction transaction(*database_);
    {
        Statement receipt(
            *database_,
            "SELECT CASE WHEN length(request)<=131072 THEN request END, CASE WHEN length(response)<=131072 THEN response END FROM research_project_commands WHERE id=?");
        receipt.bind(1, request.id());
        if (receipt.step()) {
            if (receipt.blob(0) != bytes)
                throw Error(
                    ErrorCode::conflict,
                    "project request id reused with different content");
            proto::ProjectReply replay;
            if (!replay.ParseFromString(receipt.blob(1)))
                throw std::invalid_argument("invalid research project receipt");
            validate_receipt(request, replay, owner_);
            return replay; // Read-only rollback; never rewrite the original receipt.
        }
    }
    proto::ProjectReply result;
    const auto now = clock_->utc_now() / 1000000;
    if (request.has_save()) {
        const auto& save = request.save();
        validate_id(save.project_id());
        definition(save.definition());
        const auto original = draft(save.project_id());
        if ((original ? original->digest() : std::string{}) != save.expected_digest())
            throw Error(ErrorCode::conflict, "research project draft changed");
        auto& next = *result.mutable_draft();
        next.set_id(save.project_id());
        *next.mutable_owner() = owner_;
        *next.mutable_definition() = save.definition();
        next.set_updated_at_ms(now);
        next.set_digest(draft_digest(next));
        validate(next, owner_);
        Statement persist(
            *database_,
            "INSERT INTO research_project_drafts(id,body) VALUES(?,?) ON CONFLICT(id) DO UPDATE SET body=excluded.body");
        persist.bind(1, next.id()).bind_blob(2, next.SerializeAsString()).step();
    } else if (request.has_fix()) {
        const auto& fix = request.fix();
        validate_id(fix.plan_id());
        validate_id(fix.project_id());
        const auto selected = draft(fix.project_id());
        if (!selected)
            throw Error(ErrorCode::not_found, "unknown research project");
        if (selected->digest() != fix.expected_draft_digest())
            throw Error(ErrorCode::conflict, "research project draft changed");
        if (blank(selected->definition().question()) || blank(selected->definition().hypothesis()))
            throw std::invalid_argument("research project question and hypothesis required");
        Statement existing(*database_, "SELECT 1 FROM research_hypothesis_plans WHERE id=?");
        existing.bind(1, fix.plan_id());
        if (existing.step())
            throw Error(ErrorCode::conflict, "research hypothesis plan already exists");
        if (!fix.parent_plan_id().empty()) {
            if (blank(fix.revision_reason()))
                throw std::invalid_argument("hypothesis revision reason required");
            if (plan(fix.parent_plan_id()).project_id() != fix.project_id())
                throw Error(ErrorCode::conflict, "hypothesis revision parent mismatch");
        }
        auto& next = *result.mutable_plan();
        next.set_id(fix.plan_id());
        next.set_project_id(fix.project_id());
        *next.mutable_owner() = owner_;
        *next.mutable_definition() = selected->definition();
        next.set_recorded_at_ms(now);
        next.set_parent_plan_id(fix.parent_plan_id());
        next.set_revision_reason(fix.revision_reason());
        next.set_exploratory(
            blank(next.definition().support()) || blank(next.definition().refutation()));
        next.set_definition_digest(plan_digest(next));
        validate(next, owner_);
        Statement persist(
            *database_,
            "INSERT INTO research_hypothesis_plans(id,project_id,body) VALUES(?,?,?)");
        persist.bind(1, next.id())
            .bind(2, next.project_id())
            .bind_blob(3, next.SerializeAsString())
            .step();
    } else {
        const auto& declaration = request.conclude();
        verify_binding(declaration.plan());
        const auto original = conclusion_source(*database_, declaration.result());
        Statement exists(*database_, "SELECT 1 FROM research_project_conclusions WHERE id=?");
        exists.bind(1, declaration.id());
        if (exists.step())
            throw Error(ErrorCode::conflict, "research conclusion already exists");
        if (!declaration.previous_id().empty()) {
            const auto previous = conclusion(declaration.previous_id());
            if (previous.declaration().plan().SerializeAsString()
                    != declaration.plan().SerializeAsString()
                || previous.declaration().result().SerializeAsString()
                    != declaration.result().SerializeAsString())
                throw Error(ErrorCode::conflict, "research conclusion revision mismatch");
        }
        auto& next = *result.mutable_conclusion();
        *next.mutable_declaration() = declaration;
        next.set_input_sha256(original.input_digest());
        next.set_program_artifact(original.task().program_artifact());
        next.set_retrospective(
            original.task().research_plan().SerializeAsString()
            != declaration.plan().SerializeAsString());
        next.set_recorded_at_ms(now);
        next.set_digest(conclusion_digest(next));
        validate(next, owner_);
        Statement persist(
            *database_,
            "INSERT INTO research_project_conclusions(id,project_id,body) VALUES(?,?,?)");
        persist.bind(1, declaration.id())
            .bind(2, declaration.plan().project_id())
            .bind_blob(3, next.SerializeAsString())
            .step();
    }
    validate_receipt(request, result, owner_);
    Statement receipt(
        *database_,
        "INSERT INTO research_project_commands(id,request,response) VALUES(?,?,?)");
    receipt.bind(1, request.id())
        .bind_blob(2, bytes)
        .bind_blob(3, result.SerializeAsString())
        .step();
    transaction.commit();
    return result;
}

resource::proto::Catalogue Projects::resources(const resource::proto::CatalogueQuery& query) const
{
    const auto limit = protocol::catalogue_limit(query);
    resource::proto::Catalogue result;
    *result.mutable_owner() = owner_;
    result.set_kind(query.kind());
    const auto add = [&](const std::string& id,
                         const std::string& digest,
                         const std::string& name,
                         std::size_t bytes,
                         bool fixed) {
        auto& row = *result.add_items();
        *row.mutable_object()->mutable_owner() = owner_;
        row.mutable_object()->set_kind(query.kind());
        row.mutable_object()->set_id(id);
        row.mutable_object()->set_version(digest);
        row.set_name(name);
        row.set_state(
            fixed ? resource::proto::CatalogueEntry::SEALED
                  : resource::proto::CatalogueEntry::REGISTERED);
        row.set_logical_bytes(bytes);
        row.set_relative_location("tasks.sqlite");
    };
    if (query.kind() == "research_project") {
        const auto rows = drafts(query.after(), limit);
        for (const auto& row : rows)
            add(row.id(), row.digest(), row.definition().title(), row.ByteSizeLong(), false);
        if (rows.size() == limit && !drafts(rows.back().id(), 1).empty())
            result.set_next_after(rows.back().id());
    } else if (query.kind() == "research_plan") {
        const auto rows = plans("", query.after(), limit);
        for (const auto& row : rows)
            add(row.id(),
                row.definition_digest(),
                row.definition().title(),
                row.ByteSizeLong(),
                true);
        if (rows.size() == limit && !plans("", rows.back().id(), 1).empty())
            result.set_next_after(rows.back().id());
    } else if (query.kind() == "research_conclusion") {
        const auto rows = conclusions("", query.after(), limit);
        for (const auto& row : rows)
            add(row.declaration().id(),
                row.digest(),
                plan(row.declaration().plan().plan_id()).definition().title(),
                row.ByteSizeLong(),
                true);
        if (rows.size() == limit && !conclusions("", rows.back().declaration().id(), 1).empty())
            result.set_next_after(rows.back().declaration().id());
    } else
        throw Error(ErrorCode::invalid_request, "resource kind is not implemented by Research");
    return result;
}

void Projects::verify_binding(const proto::ProjectBinding& binding) const
{
    protocol::validate_project_binding(binding);
    if (binding.owner().SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "research project owner mismatch");
    const auto fixed = plan(binding.plan_id());
    if (fixed.project_id() != binding.project_id()
        || fixed.definition_digest() != binding.definition_digest())
        throw Error(ErrorCode::conflict, "research hypothesis binding mismatch");
}

void Projects::inspect_backup(
    const std::filesystem::path& root,
    const resource::proto::Owner& expected,
    std::span<const proto::ProjectBinding> required,
    const std::function<void(const proto::ProjectDraft&)>& draft_visitor,
    const std::function<void(const proto::HypothesisPlan&)>& plan_visitor,
    const std::function<void(const proto::ProjectConclusion&)>& conclusion_visitor,
    std::stop_token stop)
{
    owner(expected);
    FileLock ownership(root, "manager.lock", FileLock::Access::shared_existing);
    Database database(root / "tasks.sqlite", Database::Access::cold_read_only);
    Projects view(
        database,
        expected,
        Database::Access::cold_read_only,
        std::make_shared<SystemClock>());
    const auto check = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "Research project cold inspection interrupted");
    };
    for (const auto& binding : required) {
        check();
        view.verify_binding(binding);
    }
    std::string cursor;
    for (;;) {
        check();
        const auto rows = view.drafts(cursor, 200);
        if (rows.empty())
            break;
        for (const auto& row : rows) {
            check();
            draft_visitor(row);
        }
        cursor = rows.back().id();
    }
    cursor.clear();
    for (;;) {
        check();
        const auto rows = view.plans("", cursor, 200);
        if (rows.empty())
            break;
        for (const auto& row : rows) {
            check();
            if (!view.draft(row.project_id()))
                throw Error(ErrorCode::not_found, "unknown research project");
            if (!row.parent_plan_id().empty()
                && view.plan(row.parent_plan_id()).project_id() != row.project_id())
                throw Error(ErrorCode::conflict, "hypothesis revision parent mismatch");
            plan_visitor(row);
        }
        cursor = rows.back().id();
    }
    cursor.clear();
    for (;;) {
        check();
        const auto rows = view.conclusions("", cursor, 200);
        if (rows.empty())
            break;
        for (const auto& row : rows) {
            check();
            if (!row.declaration().previous_id().empty()) {
                const auto previous = view.conclusion(row.declaration().previous_id());
                if (previous.declaration().plan().SerializeAsString()
                        != row.declaration().plan().SerializeAsString()
                    || previous.declaration().result().SerializeAsString()
                        != row.declaration().result().SerializeAsString())
                    throw Error(ErrorCode::conflict, "research conclusion revision mismatch");
            }
            conclusion_visitor(row);
        }
        cursor = rows.back().declaration().id();
    }
    if (!inspect(database, expected))
        return;
    Statement commands(
        database,
        "SELECT id,CASE WHEN length(request)<=131072 THEN request END,CASE WHEN length(response)<=131072 THEN response END FROM research_project_commands ORDER BY id");
    while (commands.step()) {
        check();
        proto::ProjectMutation request;
        proto::ProjectReply reply;
        if (!request.ParseFromString(commands.blob(1)) || !reply.ParseFromString(commands.blob(2))
            || request.id() != commands.text(0))
            throw std::invalid_argument("invalid research project receipt");
        validate_receipt(request, reply, expected);
        if (reply.has_draft() && !view.draft(reply.draft().id()))
            throw Error(ErrorCode::not_found, "unknown research project");
        if (reply.has_plan()
            && view.plan(reply.plan().id()).SerializeAsString() != reply.plan().SerializeAsString())
            throw std::invalid_argument("invalid research project receipt");
        if (reply.has_conclusion()
            && view.conclusion(reply.conclusion().declaration().id()).SerializeAsString()
                != reply.conclusion().SerializeAsString())
            throw std::invalid_argument("invalid research project receipt");
    }
}

} // namespace asterion::research
