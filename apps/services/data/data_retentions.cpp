#include "data_retentions.hpp"
#include "data_store.hpp"
#include <asterion/database/sqlite/sqlite_database.hpp>
#include <asterion/base/error.hpp>
#include <asterion/base/id.hpp>
#include <asterion/system/durable_file.hpp>
#include <asterion/protocol/trading.hpp>
#include <set>
#include <fstream>

namespace asterion::data {
namespace {

void digest(const std::string& value)
{
    if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("retention requires a fixed content digest");
}

std::string key(resource::proto::Object value)
{
    protocol::validate_message(value);
    validate_id(value.owner().node_id());
    validate_id(value.owner().service_id());
    const auto& domain = value.owner().domain();
    if (domain != "research" && domain != "data" && domain != "trading" && domain != "market"
        && domain != "intelligence")
        throw std::invalid_argument("unsupported retention consumer domain");
    validate_id(value.kind());
    validate_id(value.id());
    digest(value.version());
    value.clear_version();
    return value.SerializeAsString();
}

proto::DataRetentionRecord record(const std::string& body, std::int64_t phase)
{
    proto::DataRetentionRecord result;
    if (!result.mutable_retention()->ParseFromString(body)
        || (phase != proto::DataRetentionRecord::PREPARED
            && phase != proto::DataRetentionRecord::CONFIRMED
            && phase != proto::DataRetentionRecord::RELEASED))
        throw std::invalid_argument("unsupported data retention record");
    protocol::validate_message(result.retention());
    result.set_phase(static_cast<proto::DataRetentionRecord::Phase>(phase));
    return result;
}

} // namespace

Retentions::Retentions(std::filesystem::path root, proto::DataOwner owner, const Store& store)
    : authority_guard_(root / "retentions.required.pb"),
      store_(store),
      owner_(std::move(owner))
{
    validate_id(owner_.node_id());
    validate_id(owner_.service_id());
    if (owner_.service_id() != store_.instance())
        throw std::invalid_argument("retention owner does not match data store");
    const auto path = root / "retentions.sqlite";
    const bool existing = std::filesystem::exists(path);
    const bool guarded = std::filesystem::exists(authority_guard_);
    if (guarded) {
        if (std::filesystem::is_symlink(authority_guard_)
            || !std::filesystem::is_regular_file(authority_guard_)
            || std::filesystem::file_size(authority_guard_) > 4096)
            throw std::invalid_argument("invalid retention authority guard");
        std::ifstream stream(authority_guard_, std::ios::binary);
        const std::string body{std::istreambuf_iterator<char>(stream), {}};
        if (!stream || body != owner_.SerializeAsString())
            throw std::invalid_argument("retention authority owner mismatch");
        if (!existing)
            throw std::runtime_error("retention authority is missing; recovery required");
    }

    ledger_ = std::make_unique<sqlite::Database>(
        path,
        sqlite::Database::Access::writer,
        [&](sqlite::Database& database) {
            if (!existing)
                return;
            sqlite::Database::Statement read(
                database,
                "SELECT body FROM retention_owner WHERE id=1");
            if (!read.step() || read.blob(0) != owner_.SerializeAsString())
                throw std::invalid_argument("data retention owner identity mismatch");
            if (!guarded) {
                sqlite::Database::Statement rows(database, "SELECT 1 FROM retentions LIMIT 1");
                if (rows.step())
                    throw std::runtime_error(
                        "retention authority guard is missing; recovery required");
            }
            sqlite::Database::Statement shape(
                database,
                "SELECT sql FROM sqlite_schema WHERE type='table' AND name='retentions'");
            if (!shape.step() || shape.text(0).find("CHECK(phase IN (1,2,3))") == std::string::npos)
                throw std::invalid_argument("unsupported data retention schema");
        });
    sqlite::Database::Transaction transaction(*ledger_);
    ledger_->execute(
        "CREATE TABLE IF NOT EXISTS retention_owner(id INTEGER PRIMARY KEY CHECK(id=1), body BLOB NOT NULL) STRICT");
    ledger_->execute(
        "CREATE TABLE IF NOT EXISTS retentions(consumer BLOB PRIMARY KEY, body BLOB NOT NULL, phase INTEGER NOT NULL CHECK(phase IN (1,2,3))) STRICT");
    ledger_->execute(
        "CREATE TABLE IF NOT EXISTS retained_versions(consumer BLOB NOT NULL REFERENCES retentions(consumer), kind INTEGER NOT NULL, version TEXT NOT NULL, PRIMARY KEY(consumer,kind,version)) STRICT");
    ledger_->execute(
        "CREATE INDEX IF NOT EXISTS retained_versions_lookup ON retained_versions(kind,version)");
    if (!existing) {
        sqlite::Database::Statement write(
            *ledger_,
            "INSERT INTO retention_owner(id,body) VALUES(1,?)");
        write.bind_blob(1, owner_.SerializeAsString()).step();
    }
    transaction.commit();
    sync_directory(root);
}

Retentions::~Retentions() = default;

proto::DataRetention Retentions::normalize(const proto::DataRetention& request) const
{
    protocol::validate_message(request);
    (void)key(request.consumer());
    if (request.source().SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "data retention source owner mismatch");
    if (!request.versions_size() || request.versions_size() > 4096)
        throw std::invalid_argument("invalid data retention version count");
    std::set<std::pair<int, std::string>> versions;
    for (const auto& version : request.versions()) {
        digest(version.version());
        if (version.kind() != proto::DataVersion::MINUTE_VERSION
            && version.kind() != proto::DataVersion::TABLE_SNAPSHOT)
            throw std::invalid_argument("unsupported data retention version kind");
        versions.emplace(version.kind(), version.version());
    }
    auto result = request;
    result.clear_versions();
    for (const auto& [kind, version] : versions) {
        auto* item = result.add_versions();
        item->set_kind(static_cast<proto::DataVersion::Kind>(kind));
        item->set_version(version);
    }
    return result;
}

proto::DataRetentionRecord Retentions::get(const resource::proto::Object& consumer) const
{
    sqlite::Database::Statement read(
        *ledger_,
        "SELECT body,phase FROM retentions WHERE consumer=?");
    read.bind_blob(1, key(consumer));
    if (!read.step())
        throw Error(ErrorCode::not_found, "data retention is not registered");
    auto result = record(read.blob(0), read.integer(1));
    if (result.retention().consumer().SerializeAsString() != consumer.SerializeAsString())
        throw Error(ErrorCode::conflict, "retention consumer already owns a different version");
    return result;
}

proto::DataRetentionRecord Retentions::prepare(const proto::DataRetention& request)
{
    const auto value = normalize(request);
    const auto identity = key(value.consumer());
    const auto body = value.SerializeAsString();
    sqlite::Database::Transaction transaction(*ledger_);
    {
        sqlite::Database::Statement read(
            *ledger_,
            "SELECT body,phase FROM retentions WHERE consumer=?");
        read.bind_blob(1, identity);
        if (read.step()) {
            if (read.blob(0) != body)
                throw Error(
                    ErrorCode::conflict,
                    "retention identity already owns different inputs");
            auto result = record(body, read.integer(1));
            if (result.phase() == proto::DataRetentionRecord::RELEASED)
                throw Error(ErrorCode::conflict, "released retention cannot be reactivated");
            transaction.commit();
            return result;
        }
    }
    for (const auto& version : value.versions()) {
        if (version.kind() == proto::DataVersion::MINUTE_VERSION)
            (void)store_.archive().get(version.version());
        else if (!store_.tables().sequence(version.version()))
            throw Error(ErrorCode::not_found, "retained table snapshot is not published");
    }
    // Publish the durable requirement before the first authoritative reference.
    // Losing the database must never turn existing holds into an empty ledger.
    if (!std::filesystem::exists(authority_guard_))
        replace_file_durably(authority_guard_, owner_.SerializeAsString());
    else
        sync_file_durably(authority_guard_);
    sqlite::Database::Statement write(
        *ledger_,
        "INSERT INTO retentions(consumer,body,phase) VALUES(?,?,1)");
    write.bind_blob(1, identity).bind_blob(2, body).step();
    for (const auto& version : value.versions()) {
        sqlite::Database::Statement retain(
            *ledger_,
            "INSERT INTO retained_versions(consumer,kind,version) VALUES(?,?,?)");
        retain.bind_blob(1, identity)
            .bind(2, static_cast<std::int64_t>(version.kind()))
            .bind(3, version.version())
            .step();
    }
    transaction.commit();
    return record(body, proto::DataRetentionRecord::PREPARED);
}

proto::DataRetentionRecord Retentions::confirm(const proto::DataRetention& request)
{
    const auto value = normalize(request);
    sqlite::Database::Transaction transaction(*ledger_);
    auto current = get(value.consumer());
    if (current.retention().SerializeAsString() != value.SerializeAsString())
        throw Error(ErrorCode::conflict, "retention confirmation changed fixed inputs");
    if (current.phase() == proto::DataRetentionRecord::RELEASED)
        throw Error(ErrorCode::conflict, "released retention cannot be confirmed");
    sqlite::Database::Statement write(*ledger_, "UPDATE retentions SET phase=2 WHERE consumer=?");
    write.bind_blob(1, key(value.consumer())).step();
    transaction.commit();
    current.set_phase(proto::DataRetentionRecord::CONFIRMED);
    return current;
}

proto::DataRetentionRecord Retentions::release(
    const resource::proto::Object& consumer,
    const resource::proto::ReleaseDecision& proof)
{
    (void)key(consumer);
    protocol::validate_message(proof);
    if (proof.state() != resource::proto::ReleaseDecision::RELEASED
        || proof.object().SerializeAsString() != consumer.SerializeAsString())
        throw Error(ErrorCode::conflict, "consumer has not durably released these references");
    sqlite::Database::Transaction transaction(*ledger_);
    auto current = get(consumer);
    const auto value = normalize(current.retention());
    if (current.retention().SerializeAsString() != value.SerializeAsString())
        throw Error(ErrorCode::conflict, "retention release changed fixed inputs");
    sqlite::Database::Statement write(*ledger_, "UPDATE retentions SET phase=3 WHERE consumer=?");
    write.bind_blob(1, key(consumer)).step();
    transaction.commit();
    current.set_phase(proto::DataRetentionRecord::RELEASED);
    return current;
}

std::vector<proto::DataRetentionRecord> Retentions::holds(const proto::DataVersion& version) const
{
    digest(version.version());
    if (version.kind() != proto::DataVersion::MINUTE_VERSION
        && version.kind() != proto::DataVersion::TABLE_SNAPSHOT)
        throw std::invalid_argument("unsupported data retention version kind");
    sqlite::Database::Statement read(
        *ledger_,
        "SELECT r.body,r.phase FROM retained_versions v JOIN retentions r USING(consumer) WHERE v.kind=? AND v.version=? AND r.phase IN (1,2) ORDER BY v.consumer LIMIT 10001");
    read.bind(1, static_cast<std::int64_t>(version.kind())).bind(2, version.version());
    std::vector<proto::DataRetentionRecord> result;
    while (read.step()) {
        if (result.size() == 10000)
            throw Error(ErrorCode::resource_exhausted, "data retention listing limit exceeded");
        result.push_back(record(read.blob(0), read.integer(1)));
    }
    return result;
}

} // namespace asterion::data
