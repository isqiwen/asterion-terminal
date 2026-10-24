#include "observation_store.hpp"
#include "data_backup.hpp"
#include "data_store.hpp"
#include "data_retentions.hpp"
#include <algorithm>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <gtest/gtest.h>
using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct Folder
{
    fs::path path = fs::temp_directory_path() / ("ast-observation-" + unique_process_id());

    Folder()
    {
        fs::create_directory(path);
    }

    ~Folder()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

resource::proto::Owner owner()
{
    resource::proto::Owner o;
    o.set_node_id("fixture-node");
    o.set_service_id("data-one");
    o.set_domain("data");
    return o;
}

market::proto::ObservationCapture capture(const std::string& token = "preview-one")
{
    market::proto::ObservationCapture c;
    auto* s = c.mutable_observation();
    s->mutable_source()->set_node_id("fixture-node");
    s->mutable_source()->set_service_id("market-one");
    s->mutable_source()->set_domain("market");
    s->set_instance("market-process");
    s->set_token(token);
    s->set_captured_at_ms(1790000000000);
    s->set_stream_id("stream-one");
    s->set_applied_sequence(42);
    s->set_source_phase("disconnected");
    auto* m = s->mutable_minutes();
    m->mutable_instrument()->set_venue("SHFE");
    m->mutable_instrument()->set_symbol("rb2610");
    m->set_trading_day("2026-09-21");
    m->set_first_observation_ms(1789999980000);
    m->set_interrupted(true);
    auto* b = m->add_bars();
    b->set_start_ms(1789999980000);
    b->set_open("100");
    b->set_high("102");
    b->set_low("99");
    b->set_close("101");
    b->set_volume(7);
    auto* r = c.mutable_reference();
    *r->mutable_owner() = s->source();
    r->set_kind("market_observation");
    r->set_id(token);
    r->set_version(sha256_bytes(s->SerializeAsString()));
    return c;
}

data::proto::ObservationAdmission
admission(const market::proto::ObservationCapture& c, const std::string& id = "archive-one")
{
    data::proto::ObservationAdmission a;
    a.set_id(id);
    *a.mutable_source() = c.reference();
    return a;
}

} // namespace

TEST(ObservationArchive, FixedPublishedAggregateSurvivesSourceLossAndCannotBecomeCompleteHistory)
{
    Folder f;
    data::Observations store(f.path, owner());
    const auto fixed = capture();
    EXPECT_EQ(store.admit(admission(fixed)).state(), data::proto::ObservationOperation::ADMITTED);
    EXPECT_EQ(
        store.admit(admission(fixed)).source().SerializeAsString(),
        fixed.reference().SerializeAsString());
    EXPECT_THROW(store.admit(admission(capture("different"))), Error);
    unsigned reads = 0;
    const auto prepared = store.prepare("archive-one", [&](const auto& source, std::stop_token) {
        ++reads;
        EXPECT_EQ(source.SerializeAsString(), fixed.reference().SerializeAsString());
        return fixed;
    });
    const auto published = store.publish(prepared);
    EXPECT_EQ(published.state(), data::proto::ObservationOperation::PUBLISHED);
    EXPECT_GT(published.admitted_at_ms(), 0);
    EXPECT_GT(published.published_at_ms(), 0);
    EXPECT_EQ(published.archive().owner().SerializeAsString(), owner().SerializeAsString());
    data::Observations restarted(f.path, owner());
    const auto replay = restarted.prepare(
        "archive-one",
        [](const auto&, std::stop_token) -> market::proto::ObservationCapture {
            throw std::logic_error("source must not be contacted");
        });
    EXPECT_EQ(restarted.publish(replay).SerializeAsString(), published.SerializeAsString());
    EXPECT_EQ(reads, 1U);
    const auto stored = restarted.read("archive-one");
    EXPECT_EQ(stored.capture().SerializeAsString(), fixed.SerializeAsString());
    const auto view = protocol::decode_observation(stored.capture());
    EXPECT_TRUE(view.at("aggregate_only"));
    EXPECT_FALSE(view.at("complete_history"));
    EXPECT_TRUE(stored.capture().observation().minutes().interrupted());
    EXPECT_EQ(stored.capture().observation().minutes().first_observation_ms(), 1789999980000);
    EXPECT_EQ(
        std::distance(
            fs::directory_iterator(f.path / "observations/objects"),
            fs::directory_iterator{}),
        1);
    replace_file_durably(
        f.path / "observations/objects" / (fixed.reference().version() + ".pb"),
        "corrupt synthetic payload");
    EXPECT_THROW(restarted.read("archive-one"), std::invalid_argument);
}

TEST(ObservationArchive, PreparedBytesAndPublishedObjectResumeTheOriginalReceiptWithoutRefreezing)
{
    Folder f;
    const auto fixed = capture();
    data::Observations first(f.path, owner());
    first.admit(admission(fixed));
    (void)first.prepare("archive-one", [&](const auto&, std::stop_token) {
        return fixed;
    });
    data::Observations second(f.path, owner());
    const auto unavailable = [](const auto&, std::stop_token) -> market::proto::ObservationCapture {
        throw Error(ErrorCode::not_found, "source cache is gone");
    };
    auto prepared = second.prepare("archive-one", unavailable);
    // Simulate a crash after object publication, before the authoritative operation receipt.
    create_directories_durably(f.path / "observations/objects");
    publish_file_durably(
        f.path / "observations/staging/archive-one.pb",
        f.path / "observations/objects" / (fixed.reference().version() + ".pb"));
    EXPECT_EQ(
        second.read("archive-one").operation().state(),
        data::proto::ObservationOperation::ADMITTED);
    data::Observations third(f.path, owner());
    prepared = third.prepare("archive-one", unavailable);
    EXPECT_EQ(third.publish(prepared).state(), data::proto::ObservationOperation::PUBLISHED);
    EXPECT_EQ(third.read("archive-one").capture().SerializeAsString(), fixed.SerializeAsString());
}

TEST(ObservationArchive, MissingPreviewCancellationAndWrongOwnerKeepTheFixedUnfinishedDecision)
{
    Folder f;
    data::Observations store(f.path, owner());
    const auto fixed = capture();
    store.admit(admission(fixed));
    unsigned calls = 0;
    auto missing = [&](const auto&, std::stop_token) -> market::proto::ObservationCapture {
        ++calls;
        throw Error(ErrorCode::not_found, "source cache is gone");
    };
    EXPECT_THROW(store.prepare("archive-one", missing), Error);
    EXPECT_EQ(calls, 1U);
    std::stop_source cancel;
    cancel.request_stop();
    EXPECT_THROW(store.prepare("archive-one", missing, cancel.get_token()), Error);
    EXPECT_EQ(calls, 1U);
    EXPECT_THROW(
        store.prepare(
            "archive-one",
            [&](const auto&, std::stop_token) {
                return capture("new-preview");
            }),
        Error);
    EXPECT_EQ(
        store.read("archive-one").operation().source().SerializeAsString(),
        fixed.reference().SerializeAsString());
    EXPECT_EQ(
        store.read("archive-one").operation().state(),
        data::proto::ObservationOperation::ADMITTED);
    auto foreign = admission(fixed, "foreign");
    foreign.mutable_source()->mutable_owner()->set_node_id("another-node");
    EXPECT_THROW(store.admit(foreign), Error);
    EXPECT_FALSE(fs::exists(f.path / "observations/operations/foreign.pb"));
}

TEST(ObservationArchive, ColdRestoreKeepsPublishedAndPreparedOriginalsAndReportsAnUnreceivedSource)
{
    Folder folder;
    const auto root = folder.path / "source";
    fs::create_directory(root);
    const auto published = capture("published-preview"), staged = capture("staged-preview"),
               missing = capture("missing-preview");
    {
        data::Store domain(root, "data-one");
        data::proto::DataOwner data_owner;
        data_owner.set_node_id(owner().node_id());
        data_owner.set_service_id("data-one");
        data::Retentions references(root, data_owner, domain);
        data::Observations archive(root, owner());
        archive.admit(admission(published, "published-one"));
        archive.publish(archive.prepare("published-one", [&](const auto&, std::stop_token) {
            return published;
        }));
        archive.admit(admission(staged, "staged-one"));
        (void)archive.prepare("staged-one", [&](const auto&, std::stop_token) {
            return staged;
        });
        archive.admit(admission(missing, "missing-one"));
        resource::proto::CatalogueQuery query;
        query.set_kind("observation_archive");
        query.set_limit(64);
        const auto catalogue = domain.resources(owner(), query, {});
        ASSERT_EQ(catalogue.items_size(), 1);
        EXPECT_EQ(catalogue.items(0).object().id(), published.reference().version());
        EXPECT_EQ(catalogue.items(0).name(), "published-one");
    }
    data::proto::DataOwner data_owner;
    data_owner.set_node_id(owner().node_id());
    data_owner.set_service_id("data-one");
    auto exported = data::cold_backup(root, data_owner);
    EXPECT_TRUE(std::ranges::any_of(exported.objects, [&](const auto& object) {
        return object.kind() == "observation_archive"
            && object.id() == published.reference().version();
    }));
    ASSERT_EQ(exported.snapshot.dependencies.size(), 1U);
    EXPECT_EQ(
        exported.snapshot.dependencies[0].object.SerializeAsString(),
        missing.reference().SerializeAsString());
    EXPECT_FALSE(exported.snapshot.dependencies[0].included);
    EXPECT_TRUE(std::ranges::any_of(exported.snapshot.files, [](const auto& file) {
        return file.relative == "observations/staging/staged-one.pb";
    }));
    std::vector<backup_files::Snapshot> sources;
    sources.push_back(std::move(exported.snapshot));
    backup_files::Cut cut(std::move(sources));
    const auto plan = cut.preview("observation-cut");
    const auto saved = folder.path / "backup", restored = folder.path / "restored";
    (void)cut.save("observation-cut", saved, sha256_bytes(plan.dump()));
    (void)backup_files::restore_files(saved, restored);
    fs::rename(root, folder.path / "original-preserved");
    data::Observations copy(restored / "owners/0", owner());
    EXPECT_EQ(
        copy.read("published-one").capture().SerializeAsString(),
        published.SerializeAsString());
    const auto unavailable = [](const auto&, std::stop_token) -> market::proto::ObservationCapture {
        throw Error(ErrorCode::not_found, "original preview is unavailable");
    };
    auto ready = copy.prepare("staged-one", unavailable);
    copy.publish(ready);
    EXPECT_EQ(copy.read("staged-one").capture().SerializeAsString(), staged.SerializeAsString());
    EXPECT_THROW(copy.prepare("missing-one", unavailable), Error);
    EXPECT_EQ(
        copy.read("missing-one").operation().state(),
        data::proto::ObservationOperation::ADMITTED);
}

TEST(ObservationArchive, MissingAuthorityCannotBeRecreatedAsAnEmptyArchive)
{
    Folder folder;
    data::Observations archive(folder.path, owner());
    const auto fixed = capture();
    archive.admit(admission(fixed));
    const auto receipt = folder.path / "observations/operations/archive-one.pb";
    const auto before = sha256_file(receipt);
    fs::rename(folder.path / "observations", folder.path / "original-observations");
    EXPECT_THROW(archive.operations(), std::invalid_argument);
    EXPECT_THROW(
        archive.admit(admission(capture("new-source"), "new-operation")),
        std::invalid_argument);
    EXPECT_FALSE(fs::exists(folder.path / "observations"));
    EXPECT_EQ(sha256_file(folder.path / "original-observations/operations/archive-one.pb"), before);
}
