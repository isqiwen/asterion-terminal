#include "notice_store.hpp"
#include "sqlite_database.hpp"
#include "availability.hpp"
#include <asterion/protocol/intelligence.hpp>
#include <asterion/protocol/resource.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <fstream>
#include <algorithm>
#include <map>

namespace asterion::intelligence {
namespace fs = std::filesystem;

namespace {

bool same(const google::protobuf::Message& a, const google::protobuf::Message& b)
{
    return a.SerializeAsString() == b.SerializeAsString();
}

void digest(const std::string& value)
{
    if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("notice program identity is invalid");
}

void reason(const std::string& value)
{
    if (value.empty() || value.size() > 4096 || value.find('\0') != std::string::npos)
        throw std::invalid_argument("notice operation reason is missing or invalid");
}

template <class T>
T parse(const std::string& bytes, std::size_t maximum = 262144)
{
    T value;
    if (bytes.empty() || bytes.size() > maximum || !value.ParseFromString(bytes))
        throw std::invalid_argument("notice ledger record is invalid");
    protocol::validate_message(value);
    return value;
}

std::string consumer_key(const resource::proto::Object& value)
{
    return protocol::notice_consumer_key(value);
}

proto::NoticeDelivery delivery_record(
    const proto::NoticeEvent& event,
    const resource::proto::Object& source,
    proto::NoticeDeliveryKind kind,
    std::int64_t available)
{
    proto::NoticeDelivery value;
    *value.mutable_source() = source;
    *value.mutable_event() = event.reference();
    *value.mutable_initial_publication() = event.initial_publication();
    value.set_contract(event.contract());
    value.set_rule_id(event.rule_id());
    value.set_use_scope(event.use_scope());
    value.set_kind(kind);
    value.set_available_at_ms(available);
    return value;
}

void subscription_schema(sqlite::Database& ledger)
{
    sqlite::Database::Statement tables(
        ledger,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN('notice_subscriptions','notice_delivery_batches','notice_reminders')");
    if (!tables.step() || tables.integer(0) != 3)
        throw Error(ErrorCode::recovery_required, "notice subscription authority is missing");
    sqlite::Database::Statement subscriptions(
        ledger,
        "SELECT id,body FROM notice_subscriptions LIMIT 1");
    sqlite::Database::Statement batches(
        ledger,
        "SELECT id,subscription_id,acknowledged,body FROM notice_delivery_batches LIMIT 1");
    sqlite::Database::Statement reminders(
        ledger,
        "SELECT subscription_id,position,source,body FROM notice_reminders LIMIT 1");
}

void delivery_schema(sqlite::Database& ledger)
{
    sqlite::Database::Statement tables(
        ledger,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN('notice_deliveries','notice_delivery_head')");
    if (!tables.step() || tables.integer(0) != 2)
        throw Error(ErrorCode::recovery_required, "notice delivery authority is missing");
    sqlite::Database::Statement shape(
        ledger,
        "SELECT position,source,body FROM notice_deliveries LIMIT 1");
    sqlite::Database::Statement head(
        ledger,
        "SELECT position FROM notice_delivery_head WHERE id=1");
    if (!head.step() || head.integer(0) < 0)
        throw Error(ErrorCode::recovery_required, "notice delivery last position is missing");
    subscription_schema(ledger);
}

void set_schema(sqlite::Database& ledger)
{
    sqlite::Database::Statement table(
        ledger,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='notice_sets'");
    if (!table.step())
        throw Error(ErrorCode::recovery_required, "event set authority is missing");
    sqlite::Database::Statement shape(ledger, "SELECT id,published,body FROM notice_sets LIMIT 1");
}

void change_schema(sqlite::Database& ledger)
{
    sqlite::Database::Statement table(
        ledger,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='notice_changes'");
    if (!table.step())
        throw Error(ErrorCode::recovery_required, "notice change authority is missing");
    sqlite::Database::Statement shape(
        ledger,
        "SELECT id,event_id,sequence,body FROM notice_changes LIMIT 1");
}

void consumer_schema(sqlite::Database& ledger)
{
    sqlite::Database::Statement table(
        ledger,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='notice_consumers'");
    if (!table.step())
        throw Error(ErrorCode::recovery_required, "notice custody authority is missing");
    sqlite::Database::Statement shape(
        ledger,
        "SELECT consumer_key,source,phase,body FROM notice_consumers LIMIT 1");
}

} // namespace

struct Store::Impl
{
    fs::path root;
    resource::proto::Owner owner;
    std::shared_ptr<const Clock> clock;
    unsigned capacity;
    std::unique_ptr<FileLock> ownership;
    std::unique_ptr<sqlite::Database> ledger;
    std::optional<programs::Files::Entry> programme;

    Impl(
        fs::path directory,
        resource::proto::Owner authority,
        std::shared_ptr<const Clock> time,
        unsigned maximum,
        bool cold = false)
        : root(std::move(directory)),
          owner(std::move(authority)),
          clock(std::move(time)),
          capacity(maximum)
    {
        validate_id(owner.node_id());
        validate_id(owner.service_id());
        if (owner.domain() != "intelligence" || !root.is_absolute() || fs::is_symlink(root)
            || !fs::is_directory(root) || !capacity)
            throw std::invalid_argument("notice store owner, directory or capacity is invalid");
        const auto database = root / "ledger.sqlite", guard = root / "owner.pb",
                   lock = root / "intelligence.lock";
        const bool existing = fs::exists(database), guarded = fs::exists(guard);
        if (fs::is_symlink(database) || fs::is_symlink(guard) || fs::is_symlink(lock))
            throw std::invalid_argument("notice store rejects symbolic links");
        if (guarded) {
            if (!existing || !fs::is_regular_file(guard) || fs::file_size(guard) > 4096)
                throw Error(
                    ErrorCode::recovery_required,
                    "notice ledger ownership evidence is missing");
            std::ifstream input(guard, std::ios::binary);
            const std::string bytes{std::istreambuf_iterator<char>(input), {}};
            if (input.bad() || !same(parse<resource::proto::Owner>(bytes), owner))
                throw Error(ErrorCode::conflict, "notice store owner mismatch");
        }
        if (cold && (!existing || !guarded || !fs::is_regular_file(lock)))
            throw Error(ErrorCode::recovery_required, "notice cold ownership evidence missing");
        if (!fs::exists(lock)) {
            if (existing || guarded || !fs::is_empty(root))
                throw Error(
                    ErrorCode::recovery_required,
                    "notice physical ownership lock is missing");
            ownership = std::make_unique<FileLock>(root, "intelligence.lock");
        } else
            ownership = std::make_unique<FileLock>(
                root,
                "intelligence.lock",
                cold ? FileLock::Access::shared_existing : FileLock::Access::exclusive_existing);
        if (cold) {
            ledger = std::make_unique<sqlite::Database>(
                database,
                sqlite::Database::Access::cold_read_only);
            sqlite::Database::Statement read(*ledger, "SELECT body FROM notice_owner WHERE id=1");
            if (!read.step() || read.blob(0) != owner.SerializeAsString())
                throw Error(ErrorCode::conflict, "notice cold ledger owner mismatch");
            consumer_schema(*ledger);
            change_schema(*ledger);
            set_schema(*ledger);
            delivery_schema(*ledger);
            return;
        }
        ledger = std::make_unique<sqlite::
                Database>(database, sqlite::Database::Access::writer, [&](sqlite::Database& db) {
            if (!existing)
                return;
            sqlite::Database::Statement read(db, "SELECT body FROM notice_owner WHERE id=1");
            if (!read.step() || read.blob(0) != owner.SerializeAsString())
                throw Error(ErrorCode::conflict, "notice ledger owner mismatch");
            sqlite::Database::Statement shape(
                db,
                "SELECT reference,phase,created_at,updated_at,publication_id,event_id,body FROM notices LIMIT 1");
            consumer_schema(db);
            change_schema(db);
            set_schema(db);
            delivery_schema(db);
            if (!guarded && shape.step())
                throw Error(ErrorCode::recovery_required, "notice owner guard is missing");
            if (!guarded) {
                sqlite::Database::Statement operations(
                    db,
                    "SELECT 1 FROM notice_operations LIMIT 1");
                if (operations.step())
                    throw Error(
                        ErrorCode::recovery_required,
                        "notice operation authority is missing");
                sqlite::Database::Statement consumers(db, "SELECT 1 FROM notice_consumers LIMIT 1");
                if (consumers.step())
                    throw Error(
                        ErrorCode::recovery_required,
                        "notice custody owner guard is missing");
                sqlite::Database::Statement subscriptions(
                    db,
                    "SELECT 1 FROM notice_subscriptions LIMIT 1");
                sqlite::Database::Statement deliveries(
                    db,
                    "SELECT 1 FROM notice_delivery_head WHERE position>0");
                if (subscriptions.step() || deliveries.step())
                    throw Error(
                        ErrorCode::recovery_required,
                        "notice delivery owner guard is missing");
            }
        });
        if (!existing) {
            sqlite::Database::Transaction transaction(*ledger);
            ledger->execute(
                "CREATE TABLE notice_owner(id INTEGER PRIMARY KEY CHECK(id=1),body BLOB NOT NULL) STRICT");
            ledger->execute(
                "CREATE TABLE notices(id TEXT PRIMARY KEY,reference BLOB NOT NULL,phase INTEGER NOT NULL CHECK(phase IN (1,2,3,4,5,6,7)),created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,publication_id TEXT UNIQUE,event_id TEXT UNIQUE,body BLOB NOT NULL) STRICT");
            ledger->execute(
                "CREATE TABLE notice_operations(id TEXT PRIMARY KEY,action TEXT NOT NULL,request BLOB NOT NULL,result BLOB NOT NULL) STRICT");
            ledger->execute(
                "CREATE TABLE notice_consumers(consumer_key TEXT PRIMARY KEY,source BLOB NOT NULL,phase INTEGER NOT NULL CHECK(phase IN (1,2,3)),body BLOB NOT NULL) STRICT");
            ledger->execute(
                "CREATE INDEX notice_consumer_sources ON notice_consumers(source,consumer_key)");
            ledger->execute(
                "CREATE TABLE notice_changes(id TEXT PRIMARY KEY,event_id TEXT NOT NULL,sequence INTEGER,body BLOB NOT NULL,UNIQUE(event_id,sequence)) STRICT");
            ledger->execute(
                "CREATE UNIQUE INDEX notice_pending_change ON notice_changes(event_id) WHERE sequence IS NULL");
            ledger->execute(
                "CREATE TABLE notice_sets(id TEXT PRIMARY KEY,published INTEGER NOT NULL CHECK(published IN(0,1)),body BLOB NOT NULL) STRICT");
            ledger->execute(
                "CREATE TABLE notice_delivery_head(id INTEGER PRIMARY KEY CHECK(id=1),position INTEGER NOT NULL CHECK(position>=0)) STRICT");
            ledger->execute("INSERT INTO notice_delivery_head(id,position) VALUES(1,0)");
            ledger->execute(
                "CREATE TABLE notice_deliveries(position INTEGER PRIMARY KEY CHECK(position>0),source BLOB NOT NULL UNIQUE,body BLOB NOT NULL) STRICT");
            ledger->execute(
                "CREATE TABLE notice_subscriptions(id TEXT PRIMARY KEY,body BLOB NOT NULL) STRICT");
            ledger->execute(
                "CREATE TABLE notice_delivery_batches(id TEXT PRIMARY KEY,subscription_id TEXT NOT NULL,acknowledged INTEGER NOT NULL CHECK(acknowledged IN(0,1)),body BLOB NOT NULL) STRICT");
            ledger->execute(
                "CREATE UNIQUE INDEX notice_open_delivery ON notice_delivery_batches(subscription_id) WHERE acknowledged=0");
            ledger->execute(
                "CREATE TABLE notice_reminders(subscription_id TEXT NOT NULL,position INTEGER NOT NULL CHECK(position>0),source BLOB NOT NULL,body BLOB NOT NULL,PRIMARY KEY(subscription_id,source),UNIQUE(subscription_id,position)) STRICT");
            sqlite::Database::Statement write(
                *ledger,
                "INSERT INTO notice_owner(id,body) VALUES(1,?)");
            write.bind_blob(1, owner.SerializeAsString()).step();
            transaction.commit();
        }
        if (!guarded)
            replace_file_durably(guard, owner.SerializeAsString());
    }

    std::int64_t now(std::int64_t after = 0) const
    {
        const auto value = clock->utc_now() / 1000000;
        if (value <= 0 || value < after)
            throw Error(ErrorCode::unavailable, "notice clock precedes durable evidence");
        return value;
    }

    proto::NoticeRecord get(const std::string& id) const
    {
        validate_id(id);
        sqlite::Database::Statement row(
            *ledger,
            "SELECT CASE WHEN length(body)<=262144 THEN body END,reference,phase,created_at,updated_at,publication_id,event_id FROM notices WHERE id=?");
        row.bind(1, id);
        if (!row.step())
            throw Error(ErrorCode::not_found, "notice candidate is unknown");
        auto value = parse<proto::NoticeRecord>(row.blob(0));
        protocol::validate_notice_record(value);
        if (value.reference().id() != id || !same(value.reference().owner(), owner)
            || row.blob(1) != value.reference().SerializeAsString()
            || row.integer(2) != value.state() || row.integer(3) != value.created_at_ms()
            || row.integer(4) != value.updated_at_ms()
            || row.text(5)
                != (value.has_intent() ? value.intent().reference().id() : std::string{}))
            throw std::invalid_argument("notice ledger projection differs from its decision");
        if ((value.state() == proto::PUBLISHED) != value.has_event()
            || row.text(6)
                != (value.has_event()
                            && same(
                                value.event().initial_publication(),
                                value.published().reference())
                        ? value.event().reference().id()
                        : std::string{}))
            throw Error(ErrorCode::recovery_required, "notice event authority differs");
        return value;
    }

    proto::NoticeConsumerRecord consumer(const resource::proto::Object& object) const
    {
        const auto key = consumer_key(object);
        sqlite::Database::Statement row(
            *ledger,
            "SELECT CASE WHEN length(body)<=16384 THEN body END,source,phase FROM notice_consumers WHERE consumer_key=?");
        row.bind(1, key);
        if (!row.step())
            throw Error(ErrorCode::not_found, "notice consumer is unknown");
        auto value = parse<proto::NoticeConsumerRecord>(row.blob(0));
        protocol::validate_notice_consumer_record(value);
        if (!same(value.binding().consumer(), object)
            || !same(value.binding().source().owner(), owner)
            || row.blob(1) != value.binding().source().SerializeAsString()
            || row.integer(2) != value.phase())
            throw Error(ErrorCode::conflict, "notice custody projection or identity differs");
        return value;
    }

    proto::NoticeSetRecord event_set(const std::string& id) const
    {
        validate_id(id);
        sqlite::Database::Statement row(
            *ledger,
            "SELECT published,CASE WHEN length(body)<=2097152 THEN body END FROM notice_sets WHERE id=?");
        row.bind(1, id);
        if (!row.step())
            throw Error(ErrorCode::not_found, "event set is unknown");
        const auto value = parse<proto::NoticeSetRecord>(row.blob(1), 2 * 1024 * 1024);
        protocol::validate_notice_set_record(value);
        if (value.intent().reference().id() != id
            || !same(value.intent().reference().owner(), owner)
            || value.has_published() != bool(row.integer(0)))
            throw Error(ErrorCode::recovery_required, "event set projection differs");
        return value;
    }

    std::pair<std::string, std::int64_t> custody_source(const resource::proto::Object& source) const
    {
        if (!same(source.owner(), owner))
            throw Error(ErrorCode::conflict, "notice custody source owner differs");
        if (source.kind() == "intelligence_set") {
            const auto record = event_set(source.id());
            if (!record.has_published() || !same(record.published().reference(), source))
                throw Error(ErrorCode::conflict, "event set custody publication differs");
            return {
                record.intent().payload().request().query().use_scope(),
                record.published().available_at_ms()
            };
        }
        if (source.kind() != "intelligence_publication")
            throw Error(ErrorCode::conflict, "unsupported notice custody source");
        sqlite::Database::Statement row(*ledger, "SELECT id FROM notices WHERE publication_id=?");
        row.bind(1, source.id());
        if (!row.step())
            throw Error(ErrorCode::not_found, "notice custody publication is missing");
        const auto record = get(row.text(0));
        if (!record.has_published() || !same(record.published().reference(), source))
            throw Error(ErrorCode::conflict, "notice custody publication differs");
        return {
            record.input().candidate().use_scope(),
            record.published().publication().available_at_ms()
        };
    }

    proto::NoticeSubscriptionRecord get_subscription(const std::string& id) const
    {
        validate_id(id);
        sqlite::Database::Statement row(
            *ledger,
            "SELECT CASE WHEN length(body)<=16384 THEN body END FROM notice_subscriptions WHERE id=?");
        row.bind(1, id);
        if (!row.step())
            throw Error(ErrorCode::not_found, "notice subscription is unknown");
        const auto value = parse<proto::NoticeSubscriptionRecord>(row.blob(0), 16384);
        protocol::validate_notice_subscription(value);
        if (value.reference().id() != id || !same(value.reference().owner(), owner))
            throw Error(ErrorCode::recovery_required, "notice subscription projection differs");
        return value;
    }

    proto::NoticeSubscriptionRecord checked_subscription(
        const resource::proto::Object& object) const
    {
        const auto value = get_subscription(object.id());
        if (!same(value.reference(), object))
            throw Error(ErrorCode::conflict, "notice subscription fixed identity differs");
        return value;
    }

    void put_subscription(const proto::NoticeSubscriptionRecord& value)
    {
        protocol::validate_notice_subscription(value);
        sqlite::Database::Statement update(
            *ledger,
            "UPDATE notice_subscriptions SET body=? WHERE id=?");
        update.bind_blob(1, value.SerializeAsString()).bind(2, value.reference().id()).step();
    }

    proto::NoticeDeliveryBatch get_batch(const std::string& id) const
    {
        validate_id(id);
        sqlite::Database::Statement row(
            *ledger,
            "SELECT subscription_id,acknowledged,CASE WHEN length(body)<=1048576 THEN body END FROM notice_delivery_batches WHERE id=?");
        row.bind(1, id);
        if (!row.step())
            throw Error(ErrorCode::not_found, "notice delivery operation is unknown");
        const auto value = parse<proto::NoticeDeliveryBatch>(row.blob(2), 1048576);
        protocol::validate_notice_delivery_batch(value);
        if (value.reference().id() != id || !same(value.reference().owner(), owner)
            || value.payload().request().subscription().id() != row.text(0)
            || value.acknowledged() != bool(row.integer(1)))
            throw Error(
                ErrorCode::recovery_required,
                "notice delivery operation projection differs");
        return value;
    }

    std::uint64_t delivery_head() const
    {
        sqlite::Database::Statement row(
            *ledger,
            "SELECT position FROM notice_delivery_head WHERE id=1");
        if (!row.step() || row.integer(0) < 0)
            throw Error(ErrorCode::recovery_required, "notice delivery last position is missing");
        return static_cast<std::uint64_t>(row.integer(0));
    }

    void append_delivery(
        const proto::NoticeEvent& event,
        const resource::proto::Object& source,
        proto::NoticeDeliveryKind kind,
        std::int64_t available)
    {
        const auto head = delivery_head();
        if (head >= std::uint64_t(capacity) * 2)
            throw Error(ErrorCode::resource_exhausted, "notice delivery capacity is full");
        auto value = delivery_record(event, source, kind, available);
        value.set_position(head + 1);
        protocol::validate_notice_delivery(value);
        sqlite::Database::Statement insert(
            *ledger,
            "INSERT INTO notice_deliveries(position,source,body) VALUES(?,?,?)");
        insert.bind(1, static_cast<std::int64_t>(value.position()))
            .bind_blob(2, source.SerializeAsString())
            .bind_blob(3, value.SerializeAsString())
            .step();
        sqlite::Database::Statement update(
            *ledger,
            "UPDATE notice_delivery_head SET position=? WHERE id=1");
        update.bind(1, static_cast<std::int64_t>(value.position())).step();
    }

    void put(const proto::NoticeConsumerRecord& value)
    {
        protocol::validate_notice_consumer_record(value);
        sqlite::Database::Statement write(
            *ledger,
            "UPDATE notice_consumers SET phase=?,body=? WHERE consumer_key=?");
        write.bind(1, value.phase())
            .bind_blob(2, value.SerializeAsString())
            .bind(3, consumer_key(value.binding().consumer()))
            .step();
    }

    proto::NoticeRecord checked(const resource::proto::Object& ref) const
    {
        auto value = get(ref.id());
        if (!same(ref, value.reference()))
            throw Error(ErrorCode::conflict, "notice candidate fixed identity mismatch");
        return value;
    }

    void current(const proto::NoticeRecord& record) const
    {
        if (!programme)
            throw Error(ErrorCode::unavailable, "notice current program has not been retained");
        if (programme->digest != record.input().program_artifact())
            throw Error(ErrorCode::conflict, "notice preparation belongs to a different program");
    }

    void put(const proto::NoticeRecord& value)
    {
        // Empty string is a unique value too: unprepared publication IDs use NULL.
        if (value.has_intent()) {
            sqlite::Database::Statement update(
                *ledger,
                "UPDATE notices SET phase=?,updated_at=?,publication_id=?,body=? WHERE id=?");
            update.bind(1, value.state())
                .bind(2, value.updated_at_ms())
                .bind(3, value.intent().reference().id())
                .bind_blob(4, value.SerializeAsString())
                .bind(5, value.reference().id())
                .step();
        } else {
            sqlite::Database::Statement update(
                *ledger,
                "UPDATE notices SET phase=?,updated_at=?,publication_id=NULL,body=? WHERE id=?");
            update.bind(1, value.state())
                .bind(2, value.updated_at_ms())
                .bind_blob(3, value.SerializeAsString())
                .bind(4, value.reference().id())
                .step();
        }
    }

    template <class T>
    std::optional<T> operation(
        const std::string& id,
        const std::string& action,
        const google::protobuf::Message& request) const
    {
        validate_id(id);
        sqlite::Database::Statement row(
            *ledger,
            "SELECT action,CASE WHEN length(request)<=65536 THEN request END,CASE WHEN length(result)<=262144 THEN result END FROM notice_operations WHERE id=?");
        row.bind(1, id);
        if (!row.step())
            return {};
        if (row.text(0) != action || row.blob(1) != request.SerializeAsString())
            throw Error(ErrorCode::conflict, "notice operation identity has different input");
        return parse<T>(row.blob(2));
    }

    void operation(
        const std::string& id,
        const std::string& action,
        const google::protobuf::Message& request,
        const google::protobuf::Message& result)
    {
        sqlite::Database::Statement write(
            *ledger,
            "INSERT INTO notice_operations(id,action,request,result) VALUES(?,?,?,?)");
        write.bind(1, id)
            .bind(2, action)
            .bind_blob(3, request.SerializeAsString())
            .bind_blob(4, result.SerializeAsString())
            .step();
    }
};

Store::Store(
    fs::path root,
    resource::proto::Owner owner,
    std::shared_ptr<const Clock> clock,
    unsigned capacity)
    : impl_(std::make_unique<Impl>(std::move(root), std::move(owner), std::move(clock), capacity))
{}

Store::~Store() = default;

std::unique_ptr<FileLock> Store::inspect_backup(
    const fs::path& root,
    const resource::proto::Owner& owner,
    const std::function<void(const proto::NoticeRecord&)>& inspect,
    std::stop_token stop,
    const std::function<void(const proto::NoticeConsumerRecord&)>& inspect_consumer,
    const std::function<void(const proto::NoticeChangeRecord&)>& inspect_change,
    const std::function<void(const proto::NoticeSetRecord&)>& inspect_set,
    const std::function<void(const resource::proto::Object&)>& inspect_delivery_state)
{
    Impl read(root, owner, std::make_shared<SystemClock>(), 10000, true);
    sqlite::Database::Statement rows(*read.ledger, "SELECT id FROM notices ORDER BY id");
    std::size_t expected_operations = 0, records = 0;
    std::map<std::string, proto::NoticeDelivery> expected_deliveries;
    while (rows.step()) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "notice cold inspection interrupted");
        if (++records > read.capacity)
            throw Error(ErrorCode::resource_exhausted, "notice cold record capacity exceeded");
        const auto record = read.get(rows.text(0));
        if (record.has_verification()) {
            const auto& request = record.verification().review();
            const auto saved =
                read.operation<proto::NoticeVerification>(request.id(), "review", request);
            if (!saved || !same(*saved, record.verification()))
                throw std::invalid_argument("notice cold verification operation is missing");
            ++expected_operations;
        }
        if (record.has_intent()) {
            proto::NoticePublishRequest request;
            request.set_id(record.intent().reference().id());
            *request.mutable_candidate() = record.reference();
            *request.mutable_data_owner() = record.intent().data_owner();
            request.set_reason(record.intent().payload().publication_reason());
            const auto saved =
                read.operation<proto::NoticePublicationIntent>(request.id(), "publish", request);
            if (!saved || !same(*saved, record.intent()))
                throw std::invalid_argument("notice cold publication operation is missing");
            ++expected_operations;
        }
        if (record.state() == proto::ABANDONED) {
            sqlite::Database::Statement operations(
                *read.ledger,
                "SELECT id,CASE WHEN length(request)<=65536 THEN request END,result FROM notice_operations WHERE action='abandon'");
            unsigned matches = 0;
            while (operations.step()) {
                const auto request = parse<proto::NoticeAbandonRequest>(operations.blob(1));
                if (!same(request.candidate(), record.reference()))
                    continue;
                validate_id(request.id());
                reason(request.reason());
                const auto decision = parse<resource::proto::ReleaseDecision>(operations.blob(2));
                if (request.id() != operations.text(0)
                    || !same(decision.object(), record.reference())
                    || decision.state() != resource::proto::ReleaseDecision::RELEASED)
                    throw std::invalid_argument("notice cold abandonment operation differs");
                ++matches;
            }
            if (matches != 1)
                throw std::invalid_argument("notice cold abandonment authority missing");
            ++expected_operations;
        }
        if (record.has_event()) {
            sqlite::Database::Statement root(
                *read.ledger,
                "SELECT id FROM notices WHERE event_id=?");
            root.bind(1, record.event().reference().id());
            if (!root.step() || !same(read.get(root.text(0)).event(), record.event()))
                throw Error(ErrorCode::recovery_required, "notice cold event definition missing");
        }
        if (record.has_published()) {
            const auto& published = record.published();
            expected_deliveries.emplace(
                published.reference().SerializeAsString(),
                delivery_record(
                    record.event(),
                    published.reference(),
                    proto::STATEMENT_PUBLISHED,
                    published.publication().available_at_ms()));
        }
        inspect(record);
    }
    sqlite::Database::Statement consumers(
        *read.ledger,
        "SELECT CASE WHEN length(body)<=16384 THEN body END FROM notice_consumers ORDER BY consumer_key");
    unsigned custody_count = 0;
    while (consumers.step()) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "notice cold custody inspection interrupted");
        if (++custody_count > 100000)
            throw Error(ErrorCode::resource_exhausted, "notice cold custody capacity exceeded");
        const auto value = parse<proto::NoticeConsumerRecord>(consumers.blob(0));
        const auto checked = read.consumer(value.binding().consumer());
        const auto source = read.custody_source(checked.binding().source());
        if (checked.binding().use_scope() != source.first)
            throw std::invalid_argument("notice cold consumer publication differs");
        if (inspect_consumer)
            inspect_consumer(checked);
    }

    struct EventCut
    {
        proto::NoticeEvent definition;
        resource::proto::Object head, selected;
        std::uint64_t sequence = 0;
        std::int64_t available = 0;
        bool withdrawn = false, pending = false;
    };

    std::map<std::string, EventCut> events;
    sqlite::Database::Statement changes(
        *read.ledger,
        "SELECT id,event_id,sequence,CASE WHEN length(body)<=262144 THEN body END FROM notice_changes ORDER BY event_id,sequence IS NULL,sequence,id");
    std::size_t change_count = 0;
    while (changes.step()) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "notice cold change inspection interrupted");
        if (++change_count > read.capacity)
            throw Error(ErrorCode::resource_exhausted, "notice cold change capacity exceeded");
        if (!inspect_change)
            throw Error(
                ErrorCode::unavailable,
                "notice change cold dependency adapter is required");
        const auto record = parse<proto::NoticeChangeRecord>(changes.blob(3));
        protocol::validate_notice_change_record(record);
        const auto& input = record.intent().payload().input();
        const auto& request = input.request();
        if (record.intent().reference().id() != changes.text(0) || !same(input.owner(), owner)
            || request.event().id() != changes.text(1)
            || (record.has_published() ? static_cast<std::int64_t>(record.published().sequence())
                                       : 0)
                != changes.integer(2))
            throw Error(ErrorCode::recovery_required, "notice cold change projection differs");
        auto [it, inserted] = events.try_emplace(request.event().id());
        auto& cut = it->second;
        if (inserted) {
            sqlite::Database::Statement root(
                *read.ledger,
                "SELECT id FROM notices WHERE event_id=?");
            root.bind(1, request.event().id());
            if (!root.step())
                throw Error(ErrorCode::recovery_required, "notice change event authority missing");
            cut.definition = read.get(root.text(0)).event();
            cut.head = cut.selected = cut.definition.initial_publication();
            cut.available = cut.definition.available_at_ms();
        }
        if (cut.pending || cut.withdrawn || !same(cut.definition.reference(), request.event())
            || !same(request.expected_head(), cut.head)
            || !same(record.intent().payload().affected_publication(), cut.selected)
            || input.material().evidence().request().use_scope() != cut.definition.use_scope()
            || record.intent().payload().reviewed_at_ms() < cut.available)
            throw Error(
                ErrorCode::recovery_required,
                "notice change chain or evidence scope differs");
        if (request.action() == proto::NoticeChangeRequest::CORRECT) {
            sqlite::Database::Statement replacement(
                *read.ledger,
                "SELECT id FROM notices WHERE publication_id=? AND phase=5");
            replacement.bind(1, request.replacement().id());
            if (!replacement.step())
                throw Error(ErrorCode::recovery_required, "notice correction replacement missing");
            const auto statement = read.get(replacement.text(0));
            if (!same(statement.event(), cut.definition)
                || !same(statement.published().reference(), request.replacement())
                || same(request.replacement(), cut.selected)
                || statement.published().publication().available_at_ms()
                    > record.intent().payload().reviewed_at_ms())
                throw Error(ErrorCode::recovery_required, "notice correction replacement differs");
        }
        if (record.has_published()) {
            if (record.published().sequence() != cut.sequence + 1
                || record.published().available_at_ms() < cut.available)
                throw Error(
                    ErrorCode::recovery_required,
                    "notice change sequence or availability differs");
            cut.head = record.published().reference();
            cut.sequence = record.published().sequence();
            cut.available = record.published().available_at_ms();
            cut.withdrawn = request.action() == proto::NoticeChangeRequest::WITHDRAW;
            if (!cut.withdrawn)
                cut.selected = request.replacement();
        } else
            cut.pending = true;
        if (record.has_published()) {
            const auto& published = record.published();
            expected_deliveries.emplace(
                published.reference().SerializeAsString(),
                delivery_record(
                    cut.definition,
                    published.reference(),
                    request.action() == proto::NoticeChangeRequest::CORRECT
                        ? proto::EVENT_CORRECTED
                        : proto::EVENT_WITHDRAWN,
                    published.available_at_ms()));
        }
        inspect_change(record);
    }
    sqlite::Database::Statement sets(
        *read.ledger,
        "SELECT id,published,CASE WHEN length(body)<=2097152 THEN body END FROM notice_sets ORDER BY id");
    unsigned set_count = 0;
    while (sets.step()) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "event set cold inspection interrupted");
        if (++set_count > read.capacity)
            throw Error(ErrorCode::resource_exhausted, "event set cold capacity exceeded");
        if (!inspect_set)
            throw Error(ErrorCode::unavailable, "event set cold dependency adapter is required");
        const auto record = parse<proto::NoticeSetRecord>(sets.blob(2), 2 * 1024 * 1024);
        protocol::validate_notice_set_record(record);
        if (record.intent().reference().id() != sets.text(0)
            || !same(record.intent().reference().owner(), owner)
            || record.has_published() != bool(sets.integer(1)))
            throw Error(ErrorCode::recovery_required, "event set cold projection differs");
        for (const auto& selected : record.intent().payload().request().query().events()) {
            sqlite::Database::Statement root(
                *read.ledger,
                "SELECT id FROM notices WHERE event_id=?");
            root.bind(1, selected.id());
            if (!root.step() || !same(read.get(root.text(0)).event().reference(), selected))
                throw Error(
                    ErrorCode::recovery_required,
                    "event set fixed event authority differs");
        }
        for (const auto& member : record.intent().payload().members()) {
            const auto statement = read.get(member.publication().publication().candidate().id());
            if (!same(statement.published(), member.publication())
                || !same(statement.event(), member.event().event()))
                throw Error(ErrorCode::recovery_required, "event set fixed statement differs");
            resource::proto::Object expected = member.event().event().initial_publication();
            sqlite::Database::Statement changes(
                *read.ledger,
                "SELECT body FROM notice_changes WHERE event_id=? AND sequence IS NOT NULL ORDER BY sequence DESC");
            changes.bind(1, member.event().event().reference().id());
            while (changes.step()) {
                const auto change = parse<proto::NoticeChangeRecord>(changes.blob(0));
                if (change.published().available_at_ms()
                    > record.intent().payload().request().query().as_of_ms())
                    continue;
                expected = change.published().reference();
                break;
            }
            if (!same(expected, member.event().head()))
                throw Error(
                    ErrorCode::recovery_required,
                    "event set fixed historical head differs");
        }
        inspect_set(record);
    }
    const auto delivery_head = read.delivery_head();
    if (delivery_head != expected_deliveries.size())
        throw Error(
            ErrorCode::recovery_required,
            "notice cold delivery coverage differs from publications");
    sqlite::Database::Statement deliveries(
        *read.ledger,
        "SELECT position,source,CASE WHEN length(body)<=16384 THEN body END FROM notice_deliveries ORDER BY position");
    std::map<std::uint64_t, proto::NoticeDelivery> verified_deliveries;
    std::uint64_t position = 0;
    while (deliveries.step()) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "notice cold delivery inspection interrupted");
        auto value = parse<proto::NoticeDelivery>(deliveries.blob(2), 16384);
        protocol::validate_notice_delivery(value);
        if (deliveries.integer(0) != static_cast<std::int64_t>(++position)
            || value.position() != position
            || value.source().SerializeAsString() != deliveries.blob(1))
            throw Error(
                ErrorCode::recovery_required,
                "notice cold delivery stream has an unknown range");
        const auto found = expected_deliveries.find(deliveries.blob(1));
        value.clear_position();
        if (found == expected_deliveries.end() || !same(value, found->second))
            throw Error(
                ErrorCode::recovery_required,
                "notice cold delivery differs from its semantic source");
        expected_deliveries.erase(found);
        value.set_position(position);
        verified_deliveries.emplace(position, std::move(value));
    }
    if (position != delivery_head || !expected_deliveries.empty())
        throw Error(
            ErrorCode::recovery_required,
            "notice cold delivery stream has an unknown range");
    std::map<std::string, proto::NoticeSubscriptionRecord> subscriptions;
    sqlite::Database::Statement subscription_rows(
        *read.ledger,
        "SELECT id FROM notice_subscriptions ORDER BY id");
    while (subscription_rows.step()) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "notice cold subscription inspection interrupted");
        if (subscriptions.size() >= 64 || !inspect_delivery_state)
            throw Error(
                ErrorCode::recovery_required,
                "notice cold subscription adapter or capacity is invalid");
        const auto value = read.get_subscription(subscription_rows.text(0));
        proto::NoticeSubscriptionView view;
        *view.mutable_subscription() = value;
        view.set_head(delivery_head);
        protocol::validate_notice_subscription_view(view);
        subscriptions.emplace(value.reference().id(), value);
        inspect_delivery_state(value.reference());
    }
    std::map<std::string, std::vector<proto::NoticeDeliveryBatch>> batches;
    std::map<std::pair<std::string, std::string>, proto::NoticeReminder> expected_reminders;
    sqlite::Database::Statement batch_rows(
        *read.ledger,
        "SELECT id FROM notice_delivery_batches ORDER BY id");
    std::size_t batch_count = 0;
    while (batch_rows.step()) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "notice cold delivery batch inspection interrupted");
        if (++batch_count > read.capacity || !inspect_delivery_state)
            throw Error(
                ErrorCode::recovery_required,
                "notice cold delivery batch adapter or capacity is invalid");
        const auto batch = read.get_batch(batch_rows.text(0));
        const auto& request = batch.payload().request();
        const auto& page = batch.payload().page();
        const auto found = subscriptions.find(request.subscription().id());
        if (found == subscriptions.end() || !same(found->second.reference(), request.subscription())
            || !same(found->second.definition().filter(), page.query().filter())
            || request.after() < found->second.definition().start_after()
            || page.head() > delivery_head || page.query().limit() != 64
            || page.scanned_through() - request.after() > 256
            || (page.more() && page.items_size() < 64
                && page.scanned_through() - request.after() != 256)
            || batch.payload().offered_at_ms() < found->second.created_at_ms()
            || batch.payload().offered_at_ms() > found->second.updated_at_ms())
            throw Error(
                ErrorCode::recovery_required,
                "notice cold delivery batch differs from subscription coverage");
        int matched = 0;
        for (auto cursor = request.after(); cursor < page.scanned_through();) {
            const auto& source = verified_deliveries.at(++cursor);
            if (!protocol::notice_delivery_matches(page.query().filter(), source))
                continue;
            if (matched >= page.items_size() || !same(source, page.items(matched++))
                || (matched == 64 && cursor != page.scanned_through()))
                throw Error(
                    ErrorCode::recovery_required,
                    "notice cold delivery page omits or substitutes source changes");
            proto::NoticeReminder reminder;
            *reminder.mutable_subscription() = request.subscription();
            *reminder.mutable_change() = source;
            reminder.set_first_offered_at_ms(batch.payload().offered_at_ms());
            reminder.set_historical(request.historical());
            if (!expected_reminders
                    .emplace(
                        std::make_pair(
                            request.subscription().id(),
                            source.source().SerializeAsString()),
                        reminder)
                    .second)
                throw Error(
                    ErrorCode::recovery_required,
                    "notice cold delivery batches repeat a source change");
        }
        if (matched != page.items_size())
            throw Error(
                ErrorCode::recovery_required,
                "notice cold delivery page has unrelated source changes");
        batches[request.subscription().id()].push_back(batch);
        inspect_delivery_state(batch.reference());
    }
    for (const auto& [id, subscription] : subscriptions) {
        auto& history = batches[id];
        std::ranges::sort(history, [](const auto& a, const auto& b) {
            const auto& left = a.payload().page();
            const auto& right = b.payload().page();
            if (left.query().after() != right.query().after())
                return left.query().after() < right.query().after();
            return left.scanned_through() < right.scanned_through();
        });
        auto confirmed = subscription.definition().start_after();
        unsigned pending = 0;
        for (const auto& batch : history) {
            if (!batch.acknowledged()) {
                if (++pending > 1 || !subscription.has_pending_delivery()
                    || !same(subscription.pending_delivery(), batch.reference())
                    || batch.payload().request().after() != subscription.confirmed_through())
                    throw Error(
                        ErrorCode::recovery_required,
                        "notice cold pending delivery differs from subscription");
                continue;
            }
            if (batch.payload().request().after() != confirmed)
                throw Error(
                    ErrorCode::recovery_required,
                    "notice cold confirmed delivery chain has an unknown range");
            confirmed = batch.payload().page().scanned_through();
        }
        if (confirmed != subscription.confirmed_through()
            || pending != unsigned(subscription.has_pending_delivery()))
            throw Error(
                ErrorCode::recovery_required,
                "notice cold subscription progress lacks delivery evidence");
    }
    sqlite::Database::Statement reminder_rows(
        *read.ledger,
        "SELECT subscription_id,position,source,CASE WHEN length(body)<=16384 THEN body END FROM notice_reminders ORDER BY subscription_id,position");
    while (reminder_rows.step()) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "notice cold reminder inspection interrupted");
        const auto reminder = parse<proto::NoticeReminder>(reminder_rows.blob(3), 16384);
        protocol::validate_notice_reminder(reminder);
        const auto found = expected_reminders.find({reminder_rows.text(0), reminder_rows.blob(2)});
        if (found == expected_reminders.end() || !same(found->second, reminder)
            || reminder.change().position() != std::uint64_t(reminder_rows.integer(1)))
            throw Error(
                ErrorCode::recovery_required,
                "notice cold reminder lacks its exact offered source evidence");
        expected_reminders.erase(found);
    }
    if (!expected_reminders.empty())
        throw Error(ErrorCode::recovery_required, "notice cold offered reminders are missing");
    sqlite::Database::Statement count(*read.ledger, "SELECT count(*) FROM notice_operations");
    if (!count.step() || count.integer(0) != static_cast<std::int64_t>(expected_operations))
        throw std::invalid_argument("notice cold operation ledger contains unknown evidence");
    return std::move(read.ownership);
}

proto::NoticeDeliveryPage Store::deliveries(const proto::NoticeDeliveryQuery& query) const
{
    protocol::validate_notice_delivery_query(query);
    proto::NoticeDeliveryPage result;
    *result.mutable_owner() = impl_->owner;
    *result.mutable_query() = query;
    result.set_head(impl_->delivery_head());
    result.set_through(query.has_through() ? query.through() : result.head());
    if (query.after() > result.head() || result.through() > result.head())
        throw Error(ErrorCode::conflict, "notice delivery range exceeds its confirmed head");
    const auto limit = query.limit() ? query.limit() : 64U;
    auto scanned = query.after();
    sqlite::Database::Statement rows(
        *impl_->ledger,
        "SELECT position,source,CASE WHEN length(body)<=16384 THEN body END FROM notice_deliveries WHERE position>? AND position<=? ORDER BY position LIMIT 256");
    rows.bind(1, static_cast<std::int64_t>(scanned))
        .bind(2, static_cast<std::int64_t>(result.through()));
    unsigned inspected = 0;
    while (rows.step()) {
        ++inspected;
        const auto value = parse<proto::NoticeDelivery>(rows.blob(2), 16384);
        protocol::validate_notice_delivery(value);
        if (rows.integer(0) != static_cast<std::int64_t>(scanned + 1)
            || value.position() != scanned + 1 || !same(value.source().owner(), impl_->owner)
            || value.source().SerializeAsString() != rows.blob(1))
            throw Error(
                ErrorCode::recovery_required,
                "notice delivery stream has an unknown range");
        scanned = value.position();
        if (protocol::notice_delivery_matches(query.filter(), value)) {
            *result.add_items() = value;
            if (result.items_size() == static_cast<int>(limit))
                break;
        }
    }
    if (scanned < result.through() && inspected < 256
        && result.items_size() < static_cast<int>(limit))
        throw Error(ErrorCode::recovery_required, "notice delivery stream has an unknown range");
    result.set_scanned_through(scanned);
    result.set_more(scanned < result.through());
    protocol::validate_notice_delivery_page(result);
    return result;
}

proto::NoticeDeliveryPage Store::preview_subscription(
    const proto::NoticeSubscriptionDefinition& definition) const
{
    protocol::validate_notice_subscription_definition(definition);
    proto::NoticeDeliveryQuery query;
    *query.mutable_filter() = definition.filter();
    query.set_after(definition.start_after());
    query.set_limit(64);
    return deliveries(query);
}

proto::NoticeSubscriptionView Store::subscription(const std::string& id) const
{
    proto::NoticeSubscriptionView result;
    *result.mutable_subscription() = impl_->get_subscription(id);
    result.set_head(impl_->delivery_head());
    protocol::validate_notice_subscription_view(result);
    return result;
}

proto::NoticeSubscriptions Store::subscriptions() const
{
    proto::NoticeSubscriptions result;
    *result.mutable_owner() = impl_->owner;
    result.set_head(impl_->delivery_head());
    sqlite::Database::Statement rows(
        *impl_->ledger,
        "SELECT id FROM notice_subscriptions ORDER BY id");
    while (rows.step()) {
        if (result.items_size() >= 64)
            throw Error(ErrorCode::resource_exhausted, "notice subscription capacity is full");
        *result.add_items() = subscription(rows.text(0)).subscription();
    }
    return result;
}

proto::NoticeSubscriptionView Store::create_subscription(
    const proto::NoticeSubscriptionDefinition& definition)
{
    protocol::validate_notice_subscription_definition(definition);
    try {
        const auto existing = subscription(definition.id());
        if (!same(existing.subscription().definition(), definition))
            throw Error(ErrorCode::conflict, "notice subscription identity has another definition");
        return existing;
    } catch (const Error& error) {
        if (error.code() != ErrorCode::not_found)
            throw;
    }
    const auto head = impl_->delivery_head();
    if (definition.start_after() > head)
        throw Error(ErrorCode::conflict, "notice subscription start exceeds the source head");
    sqlite::Database::Statement count(*impl_->ledger, "SELECT count(*) FROM notice_subscriptions");
    if (!count.step() || count.integer(0) >= 64)
        throw Error(ErrorCode::resource_exhausted, "notice subscription capacity is full");
    proto::NoticeSubscriptionRecord value;
    *value.mutable_definition() = definition;
    auto& reference = *value.mutable_reference();
    *reference.mutable_owner() = impl_->owner;
    reference.set_kind("intelligence_subscription");
    reference.set_id(definition.id());
    reference.set_version(sha256_bytes(definition.SerializeAsString()));
    value.set_active(true);
    value.set_revision(1);
    value.set_confirmed_through(definition.start_after());
    if (head > definition.start_after())
        value.set_gap_through(head);
    value.set_created_at_ms(impl_->now(0));
    value.set_updated_at_ms(value.created_at_ms());
    protocol::validate_notice_subscription(value);
    sqlite::Database::Transaction transaction(*impl_->ledger);
    sqlite::Database::Statement insert(
        *impl_->ledger,
        "INSERT INTO notice_subscriptions(id,body) VALUES(?,?)");
    insert.bind(1, definition.id()).bind_blob(2, value.SerializeAsString()).step();
    transaction.commit();
    return subscription(definition.id());
}

proto::NoticeSubscriptionView Store::control_subscription(
    const proto::NoticeSubscriptionControl& request)
{
    protocol::validate_message(request);
    if (!request.expected_revision())
        throw std::invalid_argument("notice subscription control requires its observed revision");
    auto value = impl_->checked_subscription(request.subscription());
    const auto head = impl_->delivery_head();
    if (request.has_gap_through()) {
        if (request.active() != value.active() || request.gap_through() > head)
            throw Error(
                ErrorCode::conflict,
                "notice subscription observed gap differs from source state");
        if (request.gap_through() <= value.confirmed_through()
            || request.gap_through() <= value.gap_through())
            return subscription(value.reference().id());
    } else if (value.active() == request.active())
        return subscription(value.reference().id());
    if (value.revision() != request.expected_revision())
        throw Error(ErrorCode::conflict, "notice subscription control revision changed");
    if (request.has_gap_through()) {
        value.set_gap_through(request.gap_through());
    } else {
        value.set_active(request.active());
        if (!value.active())
            value.set_paused_through(head);
        value.set_gap_through(head > value.confirmed_through() ? head : 0);
    }
    value.set_revision(value.revision() + 1);
    value.set_updated_at_ms(impl_->now(value.updated_at_ms()));
    sqlite::Database::Transaction transaction(*impl_->ledger);
    impl_->put_subscription(value);
    transaction.commit();
    return subscription(value.reference().id());
}

proto::NoticeDeliveryBatch Store::delivery_batch(const resource::proto::Object& object) const
{
    protocol::validate_message(object);
    const auto value = impl_->get_batch(object.id());
    if (!same(value.reference(), object))
        throw Error(ErrorCode::conflict, "notice delivery fixed operation identity differs");
    return value;
}

proto::NoticeDeliveryBatch Store::delivery_batch(const proto::NoticeDeliveryLookup& request) const
{
    protocol::validate_message(request);
    const auto subscription = impl_->checked_subscription(request.subscription());
    const auto value = impl_->get_batch(request.operation_id());
    if (!same(value.payload().request().subscription(), subscription.reference()))
        throw Error(
            ErrorCode::conflict,
            "notice delivery operation belongs to another subscription");
    return value;
}

proto::NoticeDeliveryBatch Store::offer_delivery(const proto::NoticeSubscriptionOffer& request)
{
    protocol::validate_message(request);
    validate_id(request.operation_id());
    try {
        const auto prior = impl_->get_batch(request.operation_id());
        if (!same(prior.payload().request(), request))
            throw Error(ErrorCode::conflict, "notice delivery operation has another fixed range");
        return prior;
    } catch (const Error& error) {
        if (error.code() != ErrorCode::not_found)
            throw;
    }
    auto subscription = impl_->checked_subscription(request.subscription());
    if (!subscription.active())
        throw Error(ErrorCode::conflict, "notice subscription is paused");
    if (subscription.has_pending_delivery())
        throw Error(ErrorCode::conflict, "notice subscription has an unconfirmed delivery");
    if (request.after() != subscription.confirmed_through())
        throw Error(
            ErrorCode::conflict,
            "notice delivery must start at its last confirmed position");
    if (subscription.gap_through() > subscription.confirmed_through() && !request.historical())
        throw Error(
            ErrorCode::conflict,
            "notice subscription gap requires explicit historical backfill");
    proto::NoticeDeliveryQuery query;
    *query.mutable_filter() = subscription.definition().filter();
    query.set_after(request.after());
    query.set_through(request.through());
    query.set_limit(64);
    auto page = deliveries(query);
    sqlite::Database::Statement count(
        *impl_->ledger,
        "SELECT count(*) FROM notice_delivery_batches");
    if (!count.step() || count.integer(0) >= impl_->capacity)
        throw Error(ErrorCode::resource_exhausted, "notice delivery operation capacity is full");
    proto::NoticeDeliveryBatch result;
    auto& payload = *result.mutable_payload();
    *payload.mutable_request() = request;
    *payload.mutable_page() = std::move(page);
    auto after = subscription.updated_at_ms();
    for (const auto& item : payload.page().items())
        after = std::max(after, item.available_at_ms());
    payload.set_offered_at_ms(impl_->now(after));
    auto& reference = *result.mutable_reference();
    *reference.mutable_owner() = impl_->owner;
    reference.set_kind("intelligence_delivery");
    reference.set_id(request.operation_id());
    reference.set_version(sha256_bytes(payload.SerializeAsString()));
    protocol::validate_notice_delivery_batch(result);
    *subscription.mutable_pending_delivery() = reference;
    subscription.set_revision(subscription.revision() + 1);
    subscription.set_updated_at_ms(payload.offered_at_ms());
    sqlite::Database::Transaction transaction(*impl_->ledger);
    sqlite::Database::Statement insert(
        *impl_->ledger,
        "INSERT INTO notice_delivery_batches(id,subscription_id,acknowledged,body) VALUES(?,?,0,?)");
    insert.bind(1, reference.id())
        .bind(2, subscription.reference().id())
        .bind_blob(3, result.SerializeAsString())
        .step();
    for (const auto& item : payload.page().items()) {
        proto::NoticeReminder reminder;
        *reminder.mutable_subscription() = subscription.reference();
        *reminder.mutable_change() = item;
        reminder.set_first_offered_at_ms(payload.offered_at_ms());
        reminder.set_historical(request.historical());
        protocol::validate_notice_reminder(reminder);
        sqlite::Database::Statement write(
            *impl_->ledger,
            "INSERT INTO notice_reminders(subscription_id,position,source,body) VALUES(?,?,?,?) ON CONFLICT(subscription_id,source) DO NOTHING");
        write.bind(1, subscription.reference().id())
            .bind(2, static_cast<std::int64_t>(item.position()))
            .bind_blob(3, item.source().SerializeAsString())
            .bind_blob(4, reminder.SerializeAsString())
            .step();
    }
    impl_->put_subscription(subscription);
    transaction.commit();
    return result;
}

proto::NoticeSubscriptionView Store::acknowledge_delivery(const resource::proto::Object& object)
{
    auto batch = delivery_batch(object);
    auto value = impl_->checked_subscription(batch.payload().request().subscription());
    if (batch.acknowledged())
        return subscription(value.reference().id());
    if (!value.has_pending_delivery() || !same(value.pending_delivery(), object)
        || value.confirmed_through() != batch.payload().page().query().after())
        throw Error(
            ErrorCode::conflict,
            "notice delivery acknowledgement is not the pending range");
    value.set_confirmed_through(batch.payload().page().scanned_through());
    if (value.confirmed_through() >= value.gap_through())
        value.clear_gap_through();
    value.clear_pending_delivery();
    value.set_revision(value.revision() + 1);
    value.set_updated_at_ms(
        impl_->now(std::max(value.updated_at_ms(), batch.payload().offered_at_ms())));
    batch.set_acknowledged(true);
    sqlite::Database::Transaction transaction(*impl_->ledger);
    sqlite::Database::Statement update(
        *impl_->ledger,
        "UPDATE notice_delivery_batches SET acknowledged=1,body=? WHERE id=?");
    update.bind_blob(1, batch.SerializeAsString()).bind(2, object.id()).step();
    impl_->put_subscription(value);
    transaction.commit();
    return subscription(value.reference().id());
}

proto::NoticeReminderPage Store::reminders(const proto::NoticeReminderQuery& query) const
{
    protocol::validate_message(query);
    const auto fixed = impl_->checked_subscription(query.subscription());
    proto::NoticeDeliveryQuery bounds;
    *bounds.mutable_filter() = fixed.definition().filter();
    bounds.set_after(query.after());
    bounds.set_limit(query.limit());
    protocol::validate_notice_delivery_query(bounds);
    const auto limit = query.limit() ? query.limit() : 64U;
    proto::NoticeReminderPage result;
    *result.mutable_subscription() = fixed.reference();
    result.set_after(query.after());
    sqlite::Database::Statement rows(
        *impl_->ledger,
        "SELECT position,source,CASE WHEN length(body)<=16384 THEN body END FROM notice_reminders WHERE subscription_id=? AND position>? ORDER BY position LIMIT ?");
    rows.bind(1, fixed.reference().id())
        .bind(2, static_cast<std::int64_t>(query.after()))
        .bind(3, limit + 1);
    while (rows.step()) {
        const auto value = parse<proto::NoticeReminder>(rows.blob(2), 16384);
        protocol::validate_notice_reminder(value);
        if (!same(value.subscription(), fixed.reference())
            || value.change().position() != std::uint64_t(rows.integer(0))
            || value.change().source().SerializeAsString() != rows.blob(1))
            throw Error(ErrorCode::recovery_required, "notice reminder projection differs");
        if (result.items_size() == static_cast<int>(limit)) {
            result.set_next_after(result.items(result.items_size() - 1).change().position());
            break;
        }
        *result.add_items() = value;
    }
    return result;
}

void Store::set_programme(const programs::Files::Entry& entry)
{
    digest(entry.digest);
    if (!entry.bytes)
        throw std::invalid_argument("notice retained program is empty");
    if (impl_->programme) {
        if (impl_->programme->digest != entry.digest)
            throw std::logic_error("notice host program changed during its lifetime");
        return;
    }
    programs::Files files(impl_->root / "programs", impl_->owner);
    (void)files.catalogue({}, 1);
    const auto path = files.path(entry.digest);
    if (fs::is_symlink(path) || !fs::is_regular_file(path) || fs::file_size(path) != entry.bytes)
        throw Error(ErrorCode::recovery_required, "notice retained host program is missing");
    impl_->programme = entry;
}

proto::NoticeRecord Store::get(const std::string& id) const
{
    return impl_->get(id);
}

std::vector<proto::NoticeRecord>
Store::list(const std::string& after, unsigned limit, std::optional<proto::NoticeState> state) const
{
    if (!after.empty())
        validate_id(after);
    if (!limit || limit > 64 || (state && (*state < proto::PREPARING || *state > proto::ABANDONED)))
        throw std::invalid_argument("notice list query exceeds bounds");
    sqlite::Database::Statement rows(
        *impl_->ledger,
        "SELECT id FROM notices WHERE id>? AND (?=0 OR phase=?) ORDER BY id LIMIT ?");
    rows.bind(1, after).bind(2, state ? *state : 0).bind(3, state ? *state : 0).bind(4, limit);
    std::vector<proto::NoticeRecord> result;
    while (rows.step())
        result.push_back(impl_->get(rows.text(0)));
    return result;
}

resource::proto::Catalogue
Store::resources(const resource::proto::CatalogueQuery& query, std::stop_token stop) const
{
    const auto limit = protocol::catalogue_limit(query);
    if (!query.after().empty())
        validate_id(query.after());
    resource::proto::Catalogue result;
    *result.mutable_owner() = impl_->owner;
    result.set_kind(query.kind());
    const auto add = [&](const resource::proto::Object& object,
                         const std::string& name,
                         const std::string& location,
                         std::optional<std::uint64_t> bytes) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "notice resource catalogue cancelled");
        if (result.items_size() == static_cast<int>(limit)) {
            result.set_next_after(result.items(result.items_size() - 1).object().id());
            return false;
        }
        auto& entry = *result.add_items();
        *entry.mutable_object() = object;
        entry.set_name(name);
        entry.set_relative_location(location);
        entry.set_state(resource::proto::CatalogueEntry::REGISTERED);
        if (bytes)
            entry.set_logical_bytes(*bytes);
        return true;
    };
    if (query.kind() == "intelligence_program") {
        programs::Files files(impl_->root / "programs", impl_->owner);
        for (const auto& program : files.catalogue(query.after(), limit + 1)) {
            resource::proto::Object object;
            *object.mutable_owner() = impl_->owner;
            object.set_kind(query.kind());
            object.set_id(program.digest);
            object.set_version(program.digest);
            if (!add(object, "", "programs/" + program.digest, program.bytes))
                break;
        }
    } else if (query.kind() == "intelligence_subscription"
        || query.kind() == "intelligence_delivery") {
        const bool subscription = query.kind() == "intelligence_subscription";
        sqlite::Database::Statement rows(
            *impl_->ledger,
            subscription ? "SELECT id FROM notice_subscriptions WHERE id>? ORDER BY id LIMIT ?"
                         : "SELECT id FROM notice_delivery_batches WHERE id>? ORDER BY id LIMIT ?");
        rows.bind(1, query.after()).bind(2, limit + 1);
        while (rows.step()) {
            if (subscription) {
                const auto record = impl_->get_subscription(rows.text(0));
                if (!add(
                        record.reference(),
                        record.definition().name(),
                        "ledger.sqlite",
                        record.ByteSizeLong()))
                    break;
            } else {
                const auto record = impl_->get_batch(rows.text(0));
                if (!add(
                        record.reference(),
                        record.payload().request().subscription().id(),
                        "ledger.sqlite",
                        record.ByteSizeLong()))
                    break;
            }
        }
    } else if (query.kind() == "intelligence_set"
        || query.kind() == "intelligence_set_preparation") {
        const bool published = query.kind() == "intelligence_set";
        sqlite::Database::Statement rows(
            *impl_->ledger,
            published ? "SELECT id FROM notice_sets WHERE id>? AND published=1 ORDER BY id LIMIT ?"
                      : "SELECT id FROM notice_sets WHERE id>? ORDER BY id LIMIT ?");
        rows.bind(1, query.after()).bind(2, limit + 1);
        while (rows.step()) {
            const auto record = event_set(rows.text(0));
            const auto& object =
                published ? record.published().reference() : record.intent().reference();
            if (!add(
                    object,
                    record.intent().payload().request().name(),
                    "ledger.sqlite",
                    published ? record.published().ByteSizeLong() : record.intent().ByteSizeLong()))
                break;
        }
    } else if (query.kind() == "intelligence_event") {
        sqlite::Database::Statement rows(
            *impl_->ledger,
            "SELECT id FROM notices WHERE event_id>? ORDER BY event_id LIMIT ?");
        rows.bind(1, query.after()).bind(2, limit + 1);
        while (rows.step()) {
            const auto record = impl_->get(rows.text(0));
            if (!add(
                    record.event().reference(),
                    record.event().contract(),
                    "ledger.sqlite",
                    record.event().ByteSizeLong()))
                break;
        }
    } else if (query.kind() == "intelligence_change_preparation"
        || query.kind() == "intelligence_change") {
        const bool published = query.kind() == "intelligence_change";
        sqlite::Database::Statement rows(
            *impl_->ledger,
            published
                ? "SELECT id FROM notice_changes WHERE id>? AND sequence IS NOT NULL ORDER BY id LIMIT ?"
                : "SELECT id FROM notice_changes WHERE id>? ORDER BY id LIMIT ?");
        rows.bind(1, query.after()).bind(2, limit + 1);
        while (rows.step()) {
            const auto record = change(rows.text(0));
            const auto& object =
                published ? record.published().reference() : record.intent().reference();
            if (!add(
                    object,
                    record.intent().payload().input().request().reason(),
                    "ledger.sqlite",
                    published ? record.published().ByteSizeLong() : record.intent().ByteSizeLong()))
                break;
        }
    } else {
        const bool candidate = query.kind() == "intelligence_candidate";
        const bool preparation = query.kind() == "intelligence_preparation";
        if (!candidate && !preparation && query.kind() != "intelligence_publication")
            throw Error(
                ErrorCode::invalid_request,
                "resource kind is not implemented by Intelligence");
        sqlite::Database::Statement rows(
            *impl_->ledger,
            candidate ? "SELECT id FROM notices WHERE id>? ORDER BY id LIMIT ?"
                : preparation
                ? "SELECT id FROM notices WHERE publication_id>? ORDER BY publication_id LIMIT ?"
                : "SELECT id FROM notices WHERE publication_id>? AND phase=5 ORDER BY publication_id LIMIT ?");
        rows.bind(1, query.after()).bind(2, limit + 1);
        while (rows.step()) {
            const auto record = impl_->get(rows.text(0));
            const auto& object = candidate ? record.reference()
                : preparation              ? record.intent().reference()
                                           : record.published().reference();
            const std::optional<std::uint64_t> bytes = candidate ? record.ByteSizeLong()
                : preparation ? record.intent().ByteSizeLong()
                              : record.published().ByteSizeLong();
            if (!add(object, record.input().candidate().contract(), "ledger.sqlite", bytes))
                break;
        }
    }
    return result;
}

proto::NoticeConsumerRecord Store::consumer(const resource::proto::Object& object) const
{
    auto record = impl_->consumer(object);
    const auto source = impl_->custody_source(record.binding().source());
    if (record.binding().use_scope() != source.first)
        throw Error(ErrorCode::conflict, "notice custody use differs from publication");
    return record;
}

proto::NoticeConsumerRecord Store::prepare_consumer(const proto::NoticeConsumerBinding& binding)
{
    protocol::validate_notice_consumer_binding(binding);
    if (!same(binding.source().owner(), impl_->owner))
        throw Error(ErrorCode::conflict, "notice custody source owner differs");
    const auto source = impl_->custody_source(binding.source());
    if (binding.use_scope() != source.first)
        throw Error(ErrorCode::conflict, "notice custody use differs from publication");
    try {
        const auto old = consumer(binding.consumer());
        if (!same(old.binding(), binding) || old.phase() == proto::NoticeConsumerRecord::RELEASED)
            throw Error(ErrorCode::conflict, "notice consumer identity is used or released");
        return old;
    } catch (const Error& e) {
        if (e.code() != ErrorCode::not_found)
            throw;
    }
    sqlite::Database::Statement count(*impl_->ledger, "SELECT count(*) FROM notice_consumers");
    if (!count.step() || count.integer(0) >= 100000)
        throw Error(ErrorCode::resource_exhausted, "notice custody capacity exceeded");
    proto::NoticeConsumerRecord record;
    *record.mutable_binding() = binding;
    record.set_phase(proto::NoticeConsumerRecord::PREPARED);
    record.set_created_at_ms(impl_->now(source.second));
    record.set_updated_at_ms(record.created_at_ms());
    sqlite::Database::Transaction transaction(*impl_->ledger);
    sqlite::Database::Statement write(
        *impl_->ledger,
        "INSERT INTO notice_consumers(consumer_key,source,phase,body) VALUES(?,?,?,?)");
    write.bind(1, consumer_key(binding.consumer()))
        .bind_blob(2, binding.source().SerializeAsString())
        .bind(3, record.phase())
        .bind_blob(4, record.SerializeAsString())
        .step();
    transaction.commit();
    return record;
}

proto::NoticeConsumerRecord Store::confirm_consumer(const proto::NoticeConsumerBinding& binding)
{
    protocol::validate_notice_consumer_binding(binding);
    auto record = consumer(binding.consumer());
    if (!same(record.binding(), binding) || record.phase() == proto::NoticeConsumerRecord::RELEASED)
        throw Error(ErrorCode::conflict, "notice consumer confirmation differs or is released");
    if (record.phase() == proto::NoticeConsumerRecord::CONFIRMED)
        return record;
    record.set_phase(proto::NoticeConsumerRecord::CONFIRMED);
    record.set_updated_at_ms(impl_->now(record.updated_at_ms()));
    sqlite::Database::Transaction transaction(*impl_->ledger);
    impl_->put(record);
    transaction.commit();
    return record;
}

proto::NoticeConsumerRecord Store::release_consumer(
    const resource::proto::Object& object,
    const resource::proto::ReleaseDecision& proof)
{
    protocol::validate_message(proof);
    auto record = consumer(object);
    if (!same(proof.object(), object)
        || proof.state() != resource::proto::ReleaseDecision::RELEASED)
        throw Error(ErrorCode::conflict, "Research has not durably released the notice consumer");
    if (record.phase() == proto::NoticeConsumerRecord::RELEASED)
        return record;
    record.set_phase(proto::NoticeConsumerRecord::RELEASED);
    record.set_updated_at_ms(impl_->now(record.updated_at_ms()));
    sqlite::Database::Transaction transaction(*impl_->ledger);
    impl_->put(record);
    transaction.commit();
    return record;
}

proto::NoticeConsumerPage Store::consumers(const proto::NoticeConsumerQuery& query) const
{
    protocol::validate_message(query);
    (void)impl_->custody_source(query.source());
    const auto limit = query.limit() ? query.limit() : 64U;
    if (limit > 128)
        throw std::invalid_argument("notice consumer page exceeds capacity");
    if (!query.after().empty())
        digest(query.after());
    sqlite::Database::Statement rows(
        *impl_->ledger,
        "SELECT CASE WHEN length(body)<=16384 THEN body END FROM notice_consumers WHERE source=? AND consumer_key>? ORDER BY consumer_key LIMIT ?");
    rows.bind_blob(1, query.source().SerializeAsString()).bind(2, query.after()).bind(3, limit + 1);
    proto::NoticeConsumerPage result;
    while (rows.step()) {
        const auto value = parse<proto::NoticeConsumerRecord>(rows.blob(0));
        const auto checked = consumer(value.binding().consumer());
        if (!same(checked.binding().source(), query.source()))
            throw Error(ErrorCode::conflict, "notice consumer page source differs");
        if (result.items_size() == static_cast<int>(limit)) {
            result.set_next_after(
                consumer_key(result.items(result.items_size() - 1).binding().consumer()));
            break;
        }
        *result.add_items() = checked;
    }
    return result;
}

proto::NoticeRecord Store::stage(const PreparedNotice& prepared)
{
    if (!same(prepared.reference().owner(), impl_->owner))
        throw Error(ErrorCode::conflict, "notice preparation belongs to another owner");
    try {
        auto prior = impl_->get(prepared.reference().id());
        if (!same(prior.reference(), prepared.reference()))
            throw Error(ErrorCode::conflict, "notice candidate identity has different fixed input");
        return prior;
    } catch (const Error& e) {
        if (e.code() != ErrorCode::not_found)
            throw;
    }
    if (prepared.input().candidate().has_event()) {
        const auto definition = event(prepared.input().candidate().event());
        const auto& candidate = prepared.input().candidate();
        if (definition.contract() != candidate.contract()
            || definition.use_scope() != candidate.use_scope()
            || definition.rule_id() != candidate.rule_id())
            throw Error(ErrorCode::conflict, "notice follow-up differs from its event scope");
    }
    proto::NoticeRecord value;
    *value.mutable_reference() = prepared.reference();
    *value.mutable_input() = prepared.input();
    *value.mutable_assessment() = prepared.assessment();
    impl_->current(value);
    value.set_state(proto::PREPARING);
    value.set_created_at_ms(impl_->now(value.input().material().evidence().acquired_at_ms()));
    value.set_updated_at_ms(value.created_at_ms());
    sqlite::Database::Statement count(*impl_->ledger, "SELECT count(*) FROM notices");
    if (!count.step() || count.integer(0) >= impl_->capacity)
        throw Error(ErrorCode::resource_exhausted, "notice candidate capacity is full");
    sqlite::Database::Transaction transaction(*impl_->ledger);
    sqlite::Database::Statement write(
        *impl_->ledger,
        "INSERT INTO notices(id,reference,phase,created_at,updated_at,body) VALUES(?,?,?,?,?,?)");
    write.bind(1, value.reference().id())
        .bind_blob(2, value.reference().SerializeAsString())
        .bind(3, value.state())
        .bind(4, value.created_at_ms())
        .bind(5, value.updated_at_ms())
        .bind_blob(6, value.SerializeAsString())
        .step();
    transaction.commit();
    return value;
}

proto::NoticeRecord Store::admit(
    const resource::proto::Object& reference,
    const std::optional<data::proto::DataRetentionRecord>& held)
{
    auto value = impl_->checked(reference);
    if (value.state() == proto::ABANDONED)
        throw Error(ErrorCode::conflict, "notice preparation was abandoned");
    if (value.state() != proto::PREPARING)
        return value;
    impl_->current(value);
    if (value.input().has_dictionary_reference()) {
        data::proto::DataRetention expected;
        auto& source = *expected.mutable_source();
        source.set_node_id(value.input().material().reference().owner().node_id());
        source.set_service_id(value.input().material().reference().owner().service_id());
        *expected.mutable_consumer() = reference;
        auto& version = *expected.add_versions();
        version.set_kind(data::proto::DataVersion::TABLE_SNAPSHOT);
        version.set_version(value.input().dictionary_reference().version());
        if (!held || held->phase() != data::proto::DataRetentionRecord::CONFIRMED
            || !same(held->retention(), expected))
            throw Error(ErrorCode::conflict, "notice dictionary retention is not confirmed");
    } else if (held)
        throw Error(ErrorCode::conflict, "notice preparation has an unexpected retention");
    value.set_state(proto::DRAFT);
    value.set_updated_at_ms(impl_->now(value.updated_at_ms()));
    sqlite::Database::Transaction transaction(*impl_->ledger);
    impl_->put(value);
    transaction.commit();
    return value;
}

proto::NoticeVerification Store::review(const proto::NoticeReview& request)
{
    protocol::validate_notice_review(request);
    auto value = impl_->checked(request.candidate());
    if (const auto old =
            impl_->operation<proto::NoticeVerification>(request.id(), "review", request)) {
        if (!value.has_verification() || !same(*old, value.verification()))
            throw std::invalid_argument("notice review index differs from its decision");
        return *old;
    }
    impl_->current(value);
    if (value.state() != proto::DRAFT)
        throw Error(ErrorCode::conflict, "notice candidate is not awaiting human review");
    if (request.action() == proto::NoticeReview::APPROVE
        && !value.assessment().ready_for_human_review())
        throw Error(ErrorCode::conflict, "notice evidence checks still block human approval");
    auto& verified = *value.mutable_verification();
    *verified.mutable_review() = request;
    verified.set_program_artifact(impl_->programme->digest);
    verified.set_verified_at_ms(impl_->now(value.updated_at_ms()));
    value.set_state(
        request.action() == proto::NoticeReview::APPROVE ? proto::VERIFIED : proto::REJECTED);
    value.set_updated_at_ms(verified.verified_at_ms());
    sqlite::Database::Transaction transaction(*impl_->ledger);
    impl_->put(value);
    impl_->operation(request.id(), "review", request, verified);
    transaction.commit();
    return verified;
}

proto::NoticePublicationIntent Store::begin_publication(const proto::NoticePublishRequest& request)
{
    protocol::validate_message(request);
    validate_id(request.id());
    reason(request.reason());
    auto value = impl_->checked(request.candidate());
    if (const auto old =
            impl_->operation<proto::NoticePublicationIntent>(request.id(), "publish", request)) {
        if (!value.has_intent() || !same(*old, value.intent()))
            throw std::invalid_argument("notice publication index differs from its intent");
        return *old;
    }
    impl_->current(value);
    if (value.state() != proto::VERIFIED)
        throw Error(ErrorCode::conflict, "notice has no unpublished human verification");
    if (!same(request.data_owner(), value.input().material().reference().owner()))
        throw Error(
            ErrorCode::conflict,
            "notice publication Data differs from its selected source owner");
    auto& intent = *value.mutable_intent();
    auto& payload = *intent.mutable_payload();
    *payload.mutable_owner() = impl_->owner;
    payload.set_id(request.id());
    *payload.mutable_candidate() = value.reference();
    *payload.mutable_input() = value.input();
    *payload.mutable_assessment() = value.assessment();
    *payload.mutable_verification() = value.verification();
    payload.set_processed_at_ms(value.created_at_ms());
    payload.set_publication_reason(request.reason());
    auto& ref = *intent.mutable_reference();
    *ref.mutable_owner() = impl_->owner;
    ref.set_kind("intelligence_preparation");
    ref.set_id(request.id());
    ref.set_version(sha256_bytes(payload.SerializeAsString()));
    *intent.mutable_data_owner() = request.data_owner();
    intent.set_prepared_at_ms(impl_->now(value.updated_at_ms()));
    protocol::validate_notice_intent(intent);
    value.set_state(proto::PUBLISHING);
    value.set_updated_at_ms(intent.prepared_at_ms());
    sqlite::Database::Transaction transaction(*impl_->ledger);
    impl_->put(value);
    impl_->operation(request.id(), "publish", request, intent);
    transaction.commit();
    return intent;
}

proto::NoticePublished Store::confirm_publication(
    const resource::proto::Object& candidate,
    const proto::NoticeStorageReceipt& storage)
{
    protocol::validate_notice_storage(storage);
    auto value = impl_->checked(candidate);
    if (value.state() == proto::PUBLISHED) {
        if (!same(value.published().publication().storage(), storage))
            throw Error(
                ErrorCode::conflict,
                "notice publication already has another storage receipt");
        return value.published();
    }
    impl_->current(value);
    if (value.state() != proto::PUBLISHING)
        throw Error(ErrorCode::conflict, "notice publication has no durable intent");
    const auto& intent = value.intent();
    if (!same(storage.origin(), intent.reference())
        || !same(storage.content().owner(), intent.data_owner())
        || storage.bytes() != intent.payload().ByteSizeLong()
        || storage.stored_at_ms() < intent.prepared_at_ms())
        throw Error(ErrorCode::conflict, "notice Data storage differs from its publication intent");
    auto& published = *value.mutable_published();
    auto& record = *published.mutable_publication();
    *record.mutable_owner() = impl_->owner;
    record.set_id(intent.reference().id());
    *record.mutable_candidate() = candidate;
    *record.mutable_preparation() = intent.reference();
    *record.mutable_storage() = storage;
    record.set_available_at_ms(impl_->now(std::max(value.updated_at_ms(), storage.stored_at_ms())));
    auto& ref = *published.mutable_reference();
    *ref.mutable_owner() = impl_->owner;
    ref.set_kind("intelligence_publication");
    ref.set_id(record.id());
    ref.set_version(sha256_bytes(record.SerializeAsString()));
    protocol::validate_notice_published(published);
    value.set_state(proto::PUBLISHED);
    value.set_updated_at_ms(record.available_at_ms());
    const auto& input = value.input().candidate();
    if (input.has_event())
        *value.mutable_event() = event(input.event());
    else {
        auto& definition = *value.mutable_event();
        *definition.mutable_initial_publication() = published.reference();
        definition.set_contract(input.contract());
        definition.set_use_scope(input.use_scope());
        definition.set_rule_id(input.rule_id());
        definition.set_available_at_ms(record.available_at_ms());
        const auto version = sha256_bytes(definition.SerializeAsString());
        auto& identity = *definition.mutable_reference();
        *identity.mutable_owner() = impl_->owner;
        identity.set_kind("intelligence_event");
        identity.set_id(unique_process_id());
        identity.set_version(version);
    }
    protocol::validate_notice_record(value);
    sqlite::Database::Transaction transaction(*impl_->ledger);
    impl_->put(value);
    if (!input.has_event()) {
        sqlite::Database::Statement root(
            *impl_->ledger,
            "UPDATE notices SET event_id=? WHERE id=?");
        root.bind(1, value.event().reference().id()).bind(2, candidate.id()).step();
    }
    impl_->append_delivery(
        value.event(),
        published.reference(),
        proto::STATEMENT_PUBLISHED,
        record.available_at_ms());
    transaction.commit();
    return published;
}

proto::NoticePublicationIntent Store::preparation(const resource::proto::Object& ref) const
{
    sqlite::Database::Statement row(
        *impl_->ledger,
        "SELECT id FROM notices WHERE publication_id=?");
    row.bind(1, ref.id());
    if (!row.step())
        throw Error(ErrorCode::not_found, "notice publication preparation is unknown");
    auto value = impl_->get(row.text(0));
    if (!value.has_intent() || !same(value.intent().reference(), ref))
        throw Error(ErrorCode::conflict, "notice publication preparation identity differs");
    return value.intent();
}

proto::NoticePublished Store::publication(const resource::proto::Object& ref) const
{
    sqlite::Database::Statement row(
        *impl_->ledger,
        "SELECT id FROM notices WHERE publication_id=?");
    row.bind(1, ref.id());
    if (!row.step())
        throw Error(ErrorCode::not_found, "notice publication is unknown");
    auto value = impl_->get(row.text(0));
    if (value.state() != proto::PUBLISHED)
        throw Error(ErrorCode::unavailable, "notice publication is awaiting Data confirmation");
    if (!same(value.published().reference(), ref))
        throw Error(ErrorCode::conflict, "notice publication fixed identity differs");
    return value.published();
}

proto::NoticeEvent Store::event(const resource::proto::Object& reference) const
{
    if (!same(reference.owner(), impl_->owner) || reference.kind() != "intelligence_event")
        throw Error(ErrorCode::conflict, "notice event belongs to another owner or kind");
    validate_id(reference.id());
    sqlite::Database::Statement row(*impl_->ledger, "SELECT id FROM notices WHERE event_id=?");
    row.bind(1, reference.id());
    if (!row.step())
        throw Error(ErrorCode::not_found, "notice event is unknown");
    const auto root = impl_->get(row.text(0));
    if (!root.has_event() || !same(root.event().reference(), reference))
        throw Error(ErrorCode::conflict, "notice event fixed identity differs");
    return root.event();
}

proto::NoticeChangeRecord Store::change(const std::string& id) const
{
    validate_id(id);
    sqlite::Database::Statement row(
        *impl_->ledger,
        "SELECT event_id,sequence,CASE WHEN length(body)<=262144 THEN body END FROM notice_changes WHERE id=?");
    row.bind(1, id);
    if (!row.step())
        throw Error(ErrorCode::not_found, "notice change is unknown");
    const auto record = parse<proto::NoticeChangeRecord>(row.blob(2));
    protocol::validate_notice_change_record(record);
    if (record.intent().reference().id() != id
        || !same(record.intent().reference().owner(), impl_->owner)
        || record.intent().payload().input().request().event().id() != row.text(0)
        || (record.has_published() ? static_cast<std::int64_t>(record.published().sequence()) : 0)
            != row.integer(1))
        throw Error(ErrorCode::recovery_required, "notice change projection differs");
    return record;
}

proto::NoticeChangeIntent Store::change_preparation(const resource::proto::Object& ref) const
{
    const auto record = change(ref.id());
    if (!same(record.intent().reference(), ref))
        throw Error(ErrorCode::conflict, "notice change preparation identity differs");
    return record.intent();
}

proto::NoticeEventView Store::event_view(const proto::NoticeEventQuery& query) const
{
    protocol::validate_message(query);
    const auto definition = event(query.event());
    const auto cutoff = query.has_as_of_ms() ? query.as_of_ms() : impl_->now();
    if (cutoff <= 0 || cutoff > impl_->now())
        throw Error(ErrorCode::invalid_request, "notice event cutoff is in the future or invalid");
    if (cutoff < definition.available_at_ms())
        throw Error(ErrorCode::not_found, "notice event was not available at this cutoff");
    proto::NoticeEventView view;
    *view.mutable_event() = definition;
    *view.mutable_head() = definition.initial_publication();
    *view.mutable_selected_publication() = definition.initial_publication();
    sqlite::Database::Statement rows(
        *impl_->ledger,
        "SELECT id FROM notice_changes WHERE event_id=? AND sequence IS NOT NULL ORDER BY sequence DESC");
    rows.bind(1, definition.reference().id());
    while (rows.step()) {
        const auto value = change(rows.text(0));
        if (value.published().available_at_ms() > cutoff)
            continue;
        *view.mutable_head() = value.published().reference();
        *view.mutable_latest_change() = value;
        const bool withdrawn = value.intent().payload().input().request().action()
            == proto::NoticeChangeRequest::WITHDRAW;
        view.set_withdrawn(withdrawn);
        *view.mutable_selected_publication() = withdrawn
            ? value.intent().payload().affected_publication()
            : value.intent().payload().input().request().replacement();
        break;
    }
    if (!query.has_as_of_ms()) {
        sqlite::Database::Statement pending(
            *impl_->ledger,
            "SELECT id FROM notice_changes WHERE event_id=? AND sequence IS NULL");
        pending.bind(1, definition.reference().id());
        if (pending.step())
            *view.mutable_pending_change() = change(pending.text(0));
    }
    return view;
}

proto::NoticeSetPreview Store::preview_set(proto::NoticeSetRequest request) const
{
    if (!impl_->programme)
        throw Error(ErrorCode::unavailable, "event set method has not been retained");
    if (request.program_artifact().empty())
        request.set_program_artifact(impl_->programme->digest);
    protocol::validate_notice_set_request(request);
    if (!same(request.query().events(0).owner(), impl_->owner)
        || request.program_artifact() != impl_->programme->digest)
        throw Error(
            ErrorCode::conflict,
            "event set preview requires its current owner and retained method");
    if (request.query().as_of_ms() > impl_->now())
        throw Error(ErrorCode::invalid_request, "event set cutoff is in the future");
    proto::NoticeSetPreview result;
    auto& payload = *result.mutable_payload();
    *payload.mutable_owner() = impl_->owner;
    *payload.mutable_request() = request;
    const auto& query = request.query();
    for (const auto& selected : query.events()) {
        const auto definition = event(selected);
        if (definition.use_scope() != query.use_scope())
            throw Error(ErrorCode::conflict, "event set use scope differs from selected events");
        const auto excluded = [&](const resource::proto::Object& source,
                                  const std::string& reason) {
            auto& entry = *payload.add_exclusions();
            *entry.mutable_event() = selected;
            auto& why = *entry.add_reasons();
            *why.mutable_source() = source;
            why.set_reason(reason);
        };
        if (definition.available_at_ms() > query.as_of_ms()) {
            excluded(selected, "event_not_available_by_cutoff");
            continue;
        }
        proto::NoticeEventQuery selection;
        *selection.mutable_event() = selected;
        selection.set_as_of_ms(query.as_of_ms());
        const auto view = event_view(selection);
        if (view.withdrawn()) {
            excluded(view.head(), "event_withdrawn_by_cutoff");
            continue;
        }
        const auto published = publication(view.selected_publication());
        const auto statement = impl_->get(published.publication().candidate().id());
        if (!same(published.publication().storage().content().owner(), request.data_owner())
            || (view.has_latest_change()
                && !same(
                    view.latest_change().published().storage().content().owner(),
                    request.data_owner())))
            throw Error(
                ErrorCode::unavailable,
                "event set transfer between Data owners is not implemented");
        const auto assess = [&](const data::proto::MaterialReceipt& material,
                                std::int64_t processed,
                                std::int64_t verified,
                                const proto::NoticeStorageReceipt& saved,
                                std::int64_t available) {
            NoticeAvailability evidence;
            if (material.evidence().has_source_published_at_ms())
                evidence.source_published_at_ms = material.evidence().source_published_at_ms();
            evidence.acquired_at_ms = material.evidence().acquired_at_ms();
            evidence.processed_at_ms = processed;
            evidence.verified_at_ms = verified;
            evidence.stored_at_ms = saved.stored_at_ms();
            evidence.available_at_ms = available;
            evidence.as_of_ms = query.as_of_ms();
            evidence.declared_strict_history =
                statement.input().candidate().use_mode() == proto::STRICT_HISTORY;
            return review_notice_availability(evidence);
        };
        const auto original = assess(
            statement.input().material(),
            statement.created_at_ms(),
            statement.verification().verified_at_ms(),
            published.publication().storage(),
            published.publication().available_at_ms());
        bool strict = original.strict_history_eligible;
        std::vector<proto::NoticeSetLimitation> limitations;
        const auto retain = [&](const resource::proto::Object& source, const auto& review) {
            for (const auto& reason : review.limitations) {
                proto::NoticeSetLimitation item;
                *item.mutable_source() = source;
                item.set_reason(reason);
                limitations.push_back(std::move(item));
            }
        };
        retain(published.reference(), original);
        if (view.has_latest_change()) {
            const auto& change = view.latest_change();
            const auto& evidence = change.intent().payload();
            const auto relation = assess(
                evidence.input().material(),
                evidence.reviewed_at_ms(),
                evidence.reviewed_at_ms(),
                change.published().storage(),
                change.published().available_at_ms());
            strict = strict && relation.strict_history_eligible;
            retain(change.published().reference(), relation);
        }
        if (query.use_mode() == proto::STRICT_HISTORY && !strict) {
            auto& entry = *payload.add_exclusions();
            *entry.mutable_event() = selected;
            for (const auto& item : limitations)
                *entry.add_reasons() = item;
            continue;
        }
        auto& member = *payload.add_members();
        *member.mutable_event() = view;
        *member.mutable_publication() = published;
        for (const auto& item : limitations)
            *member.add_limitations() = item;
    }
    protocol::validate_notice_set_payload(payload);
    result.set_observed_at_ms(impl_->now(query.as_of_ms()));
    return result;
}

proto::NoticeSetRecord Store::event_set(const std::string& id) const
{
    return impl_->event_set(id);
}

proto::NoticeSetIntent Store::begin_set(const proto::NoticeSetRequest& request)
{
    protocol::validate_notice_set_request(request);
    try {
        const auto prior = event_set(request.id());
        if (!same(prior.intent().payload().request(), request))
            throw Error(ErrorCode::conflict, "event set operation has another fixed request");
        return prior.intent();
    } catch (const Error& error) {
        if (error.code() != ErrorCode::not_found)
            throw;
    }
    const auto preview = preview_set(request);
    if (preview.payload().members().empty())
        throw Error(ErrorCode::conflict, "event set has no usable members; inspect exclusions");
    sqlite::Database::Statement count(*impl_->ledger, "SELECT count(*) FROM notice_sets");
    if (!count.step() || count.integer(0) >= impl_->capacity)
        throw Error(ErrorCode::resource_exhausted, "event set capacity is full");
    proto::NoticeSetRecord record;
    auto& intent = *record.mutable_intent();
    *intent.mutable_payload() = preview.payload();
    *intent.mutable_data_owner() = request.data_owner();
    intent.set_prepared_at_ms(preview.observed_at_ms());
    auto& ref = *intent.mutable_reference();
    *ref.mutable_owner() = impl_->owner;
    ref.set_kind("intelligence_set_preparation");
    ref.set_id(request.id());
    ref.set_version(sha256_bytes(intent.payload().SerializeAsString()));
    protocol::validate_notice_set_record(record);
    sqlite::Database::Transaction transaction(*impl_->ledger);
    sqlite::Database::Statement write(
        *impl_->ledger,
        "INSERT INTO notice_sets(id,published,body) VALUES(?,0,?)");
    write.bind(1, request.id()).bind_blob(2, record.SerializeAsString()).step();
    transaction.commit();
    return intent;
}

proto::NoticeSetIntent Store::set_preparation(const resource::proto::Object& ref) const
{
    const auto value = event_set(ref.id());
    if (!same(value.intent().reference(), ref))
        throw Error(ErrorCode::conflict, "event set preparation identity differs");
    return value.intent();
}

proto::NoticeSetRecord Store::set_publication(const resource::proto::Object& ref) const
{
    const auto value = event_set(ref.id());
    if (!value.has_published())
        throw Error(ErrorCode::unavailable, "event set is awaiting Data confirmation");
    if (!same(value.published().reference(), ref))
        throw Error(ErrorCode::conflict, "event set fixed publication identity differs");
    return value;
}

proto::NoticeSetRecord
Store::confirm_set(const resource::proto::Object& ref, const proto::NoticeStorageReceipt& receipt)
{
    protocol::validate_notice_storage(receipt);
    auto value = event_set(ref.id());
    if (!same(value.intent().reference(), ref))
        throw Error(ErrorCode::conflict, "event set preparation identity differs");
    if (value.has_published()) {
        if (!same(value.published().storage(), receipt))
            throw Error(ErrorCode::conflict, "event set already has another Data receipt");
        return value;
    }
    const auto& intent = value.intent();
    if (!same(receipt.origin(), ref) || !same(receipt.content().owner(), intent.data_owner())
        || receipt.bytes() != intent.payload().ByteSizeLong()
        || receipt.stored_at_ms() < intent.prepared_at_ms())
        throw Error(ErrorCode::conflict, "event set Data receipt differs from its fixed intent");
    auto& published = *value.mutable_published();
    *published.mutable_preparation() = ref;
    *published.mutable_storage() = receipt;
    published.set_available_at_ms(impl_->now(receipt.stored_at_ms()));
    const auto version = sha256_bytes(published.SerializeAsString());
    auto& identity = *published.mutable_reference();
    *identity.mutable_owner() = impl_->owner;
    identity.set_kind("intelligence_set");
    identity.set_id(ref.id());
    identity.set_version(version);
    protocol::validate_notice_set_record(value);
    sqlite::Database::Transaction transaction(*impl_->ledger);
    sqlite::Database::Statement write(
        *impl_->ledger,
        "UPDATE notice_sets SET published=1,body=? WHERE id=?");
    write.bind_blob(1, value.SerializeAsString()).bind(2, ref.id()).step();
    transaction.commit();
    return value;
}

proto::NoticeChangeIntent Store::begin_change(const PreparedNoticeChange& prepared)
{
    const auto& input = prepared.input();
    protocol::validate_notice_change_input(input);
    const auto& request = input.request();
    if (!same(input.owner(), impl_->owner))
        throw Error(ErrorCode::conflict, "notice change belongs to another owner");
    try {
        const auto prior = change(request.id());
        if (!same(prior.intent().payload().input(), input))
            throw Error(ErrorCode::conflict, "notice change operation has different fixed input");
        return prior.intent();
    } catch (const Error& e) {
        if (e.code() != ErrorCode::not_found)
            throw;
    }
    if (!impl_->programme || impl_->programme->digest != input.program_artifact())
        throw Error(ErrorCode::conflict, "notice change does not use the retained host program");
    proto::NoticeEventQuery query;
    *query.mutable_event() = request.event();
    const auto view = event_view(query);
    if (view.withdrawn() || !same(view.head(), request.expected_head()))
        throw Error(ErrorCode::conflict, "notice change expected head is stale or withdrawn");
    sqlite::Database::Statement pending(
        *impl_->ledger,
        "SELECT id FROM notice_changes WHERE event_id=? AND sequence IS NULL");
    pending.bind(1, request.event().id());
    if (pending.step())
        throw Error(ErrorCode::conflict, "notice event has an unfinished change");
    const auto original = publication(view.selected_publication());
    const auto original_input = get(original.publication().candidate().id()).input();
    if (same(request.material(), original_input.material().reference())
        || input.material().evidence().request().use_scope() != view.event().use_scope())
        throw Error(
            ErrorCode::conflict,
            "notice change requires new evidence with the event use scope");
    if (request.action() == proto::NoticeChangeRequest::CORRECT) {
        const auto replacement = publication(request.replacement());
        const auto related = get(replacement.publication().candidate().id());
        if (!same(related.event(), view.event())
            || same(request.replacement(), view.selected_publication()))
            throw Error(
                ErrorCode::conflict,
                "notice correction replacement is not another published event statement");
    }
    sqlite::Database::Statement count(*impl_->ledger, "SELECT count(*) FROM notice_changes");
    if (!count.step() || count.integer(0) >= impl_->capacity)
        throw Error(ErrorCode::resource_exhausted, "notice change capacity is full");
    proto::NoticeChangeRecord record;
    auto& intent = *record.mutable_intent();
    auto& payload = *intent.mutable_payload();
    *payload.mutable_input() = input;
    *payload.mutable_affected_publication() = view.selected_publication();
    auto after =
        std::max(input.material().evidence().acquired_at_ms(), view.event().available_at_ms());
    if (view.has_latest_change())
        after = std::max(after, view.latest_change().published().available_at_ms());
    payload.set_reviewed_at_ms(impl_->now(after));
    *intent.mutable_data_owner() = input.material().reference().owner();
    intent.set_prepared_at_ms(payload.reviewed_at_ms());
    auto& ref = *intent.mutable_reference();
    *ref.mutable_owner() = impl_->owner;
    ref.set_kind("intelligence_change_preparation");
    ref.set_id(request.id());
    ref.set_version(sha256_bytes(payload.SerializeAsString()));
    protocol::validate_notice_change_record(record);
    sqlite::Database::Transaction transaction(*impl_->ledger);
    sqlite::Database::Statement write(
        *impl_->ledger,
        "INSERT INTO notice_changes(id,event_id,body) VALUES(?,?,?)");
    write.bind(1, request.id())
        .bind(2, request.event().id())
        .bind_blob(3, record.SerializeAsString())
        .step();
    transaction.commit();
    return intent;
}

proto::NoticeChangeRecord Store::confirm_change(
    const resource::proto::Object& ref,
    const proto::NoticeStorageReceipt& receipt)
{
    protocol::validate_notice_storage(receipt);
    auto record = change(ref.id());
    if (!same(record.intent().reference(), ref))
        throw Error(ErrorCode::conflict, "notice change preparation differs");
    if (record.has_published()) {
        if (!same(record.published().storage(), receipt))
            throw Error(ErrorCode::conflict, "notice change already has another Data receipt");
        return record;
    }
    const auto& intent = record.intent();
    proto::NoticeEventQuery query;
    *query.mutable_event() = intent.payload().input().request().event();
    const auto view = event_view(query);
    if (view.withdrawn() || !same(view.head(), intent.payload().input().request().expected_head())
        || !same(receipt.origin(), ref) || !same(receipt.content().owner(), intent.data_owner())
        || receipt.bytes() != intent.payload().ByteSizeLong()
        || receipt.stored_at_ms() < intent.prepared_at_ms())
        throw Error(ErrorCode::conflict, "notice change Data receipt or current head differs");
    auto& published = *record.mutable_published();
    *published.mutable_preparation() = ref;
    *published.mutable_storage() = receipt;
    published.set_available_at_ms(impl_->now(receipt.stored_at_ms()));
    published.set_sequence(
        view.has_latest_change() ? view.latest_change().published().sequence() + 1 : 1);
    const auto version = sha256_bytes(published.SerializeAsString());
    auto& identity = *published.mutable_reference();
    *identity.mutable_owner() = impl_->owner;
    identity.set_kind("intelligence_change");
    identity.set_id(ref.id());
    identity.set_version(version);
    protocol::validate_notice_change_record(record);
    sqlite::Database::Transaction transaction(*impl_->ledger);
    sqlite::Database::Statement write(
        *impl_->ledger,
        "UPDATE notice_changes SET sequence=?,body=? WHERE id=?");
    write.bind(1, static_cast<std::int64_t>(published.sequence()))
        .bind_blob(2, record.SerializeAsString())
        .bind(3, ref.id())
        .step();
    impl_->append_delivery(
        view.event(),
        published.reference(),
        intent.payload().input().request().action() == proto::NoticeChangeRequest::CORRECT
            ? proto::EVENT_CORRECTED
            : proto::EVENT_WITHDRAWN,
        published.available_at_ms());
    transaction.commit();
    return record;
}

resource::proto::ReleaseDecision Store::abandon(
    const resource::proto::Object& ref,
    const std::string& id,
    const std::string& explanation)
{
    validate_id(id);
    reason(explanation);
    auto value = impl_->checked(ref);
    proto::NoticeAbandonRequest request;
    request.set_id(id);
    *request.mutable_candidate() = ref;
    request.set_reason(explanation);
    if (const auto old =
            impl_->operation<resource::proto::ReleaseDecision>(id, "abandon", request)) {
        if (value.state() != proto::ABANDONED || !same(old->object(), ref)
            || old->state() != resource::proto::ReleaseDecision::RELEASED)
            throw std::invalid_argument("notice abandonment index differs from its decision");
        return *old;
    }
    if (value.state() != proto::PREPARING)
        throw Error(ErrorCode::conflict, "admitted notice cannot be abandoned as a preparation");
    resource::proto::ReleaseDecision result;
    *result.mutable_object() = ref;
    result.set_state(resource::proto::ReleaseDecision::RELEASED);
    value.set_state(proto::ABANDONED);
    value.set_updated_at_ms(impl_->now(value.updated_at_ms()));
    sqlite::Database::Transaction transaction(*impl_->ledger);
    impl_->put(value);
    impl_->operation(id, "abandon", request, result);
    transaction.commit();
    return result;
}

resource::proto::ReleaseDecision Store::release_decision(const resource::proto::Object& ref) const
{
    resource::proto::ReleaseDecision result;
    *result.mutable_object() = ref;
    try {
        const auto value = impl_->checked(ref);
        result.set_state(
            value.state() == proto::ABANDONED ? resource::proto::ReleaseDecision::RELEASED
                                              : resource::proto::ReleaseDecision::RETAINED);
    } catch (const Error& e) {
        if (e.code() != ErrorCode::not_found)
            throw;
        result.set_state(resource::proto::ReleaseDecision::UNKNOWN);
    }
    return result;
}

} // namespace asterion::intelligence
