#include "notice_store.hpp"
#include "research_submission.hpp"
#include "data_submission.hpp"
#include "research_execution.hpp"
#include "research_data.hpp"
#include "research_backup.hpp"
#include "data_backup.hpp"
#include "intelligence_backup.hpp"
#include "domain_backup.hpp"
#include "tasks/task_store_support.hpp"
#include <asterion/protocol/notice_review.hpp>
#include <asterion/protocol/task_execution.hpp>
#include <asterion/research.pb.h>
#include "material_store.hpp"
#include "material_providers.hpp"
#include <asterion/kernel/native_plugin.hpp>
#include "intelligence_content.hpp"
#include "data_store.hpp"
#include "data_retentions.hpp"
#include "table_series.hpp"
#include "data/history_fixture.hpp"
#include "sqlite_database.hpp"
#include <asterion/protocol/material.hpp>
#include <asterion/protocol/intelligence.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include "support/local_listener.hpp"
#include <asterion/protocol/data_client.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/trace.hpp>
#include <thread>
#include <atomic>
using namespace asterion;
namespace ip = asterion::intelligence::proto;
namespace fs = std::filesystem;

namespace {

struct Folder
{
    fs::path path = fs::temp_directory_path() / ("ast-notice-" + unique_process_id());

    Folder()
    {
        fs::create_directory(path);
    }

    ~Folder()
    {
        if (::testing::Test::HasFailure() || std::uncaught_exceptions()) {
            std::cerr << "Retained isolated notice evidence: " << path << '\n';
            return;
        }
        std::error_code e;
        fs::remove_all(path, e);
    }
};

resource::proto::Owner owner(const std::string& domain, const std::string& service)
{
    resource::proto::Owner o;
    o.set_node_id("fixture-node");
    o.set_service_id(service);
    o.set_domain(domain);
    return o;
}

struct Fixture
{
    Folder folder;
    fs::path data_root = folder.path / "data", notice_root = folder.path / "intelligence";
    resource::proto::Owner notice_owner = owner("intelligence", "intelligence-one"),
                           data_owner = owner("data", "historical-data");
    std::shared_ptr<ManualClock> clock;
    std::unique_ptr<data::Store> data;
    std::unique_ptr<data::Retentions> retentions;
    std::unique_ptr<intelligence::Store> notices;
    programs::Files::Entry programme;
    data::proto::MaterialRead material;
    data::proto::TableListedContracts dictionary;
    std::string body = "SHFE/rb/2026-11 计划于2026-10-24将保证金率调整为12%。";

    explicit Fixture(bool retain_provider = false)
    {
        fs::create_directory(data_root);
        fs::create_directory(notice_root);
        {
            data::Store initialize(data_root, "historical-data");
        }
        const auto daily = test::seed_daily_table(
            data_root,
            "SHFE/rb/2026-11",
            std::vector<std::string>{"2026-09-25"},
            110,
            100);
        data = std::make_unique<data::Store>(data_root, "historical-data");
        data::proto::DataOwner o;
        o.set_node_id("fixture-node");
        o.set_service_id("historical-data");
        retentions = std::make_unique<data::Retentions>(data_root, o, *data);
        clock = std::make_shared<ManualClock>(SystemClock{}.utc_now() - 1000000000);
        data::proto::MaterialRequest request;
        request.set_id("source-one");
        request.set_source_id("synthetic-source");
        request.set_resource_id("rule-announcement");
        request.set_use_scope("research");
        request.set_provider_artifact(std::string(64, 'a'));
        if (retain_provider) {
            configure_native_plugins(MATERIAL_FIXTURE_DIRECTORY);
            const auto sources = material_providers::sources();
            const auto source = std::ranges::find_if(sources, [](const auto& value) {
                return value.id == "fixture-material";
            });
            if (source == sources.end())
                throw std::runtime_error("isolated material provider fixture is missing");
            request.set_source_id(source->id);
            request.set_provider_artifact(source->artifact);
        }
        request.set_max_requests(1);
        request.set_max_bytes(4096);
        if (retain_provider)
            material_providers::preserve(request, data_root / "programs");
        data::Materials materials(data_root, data_owner);
        const auto receipt = materials.publish(
            materials.prepare(request, body, "text/plain", clock->utc_now() / 1000000));
        *material.mutable_receipt() = receipt;
        material.set_original(body);
        *material.mutable_text() = protocol::read_material_text(receipt, body);
        data::proto::TableListedContractsQuery query;
        query.set_snapshot(daily.at("snapshot").get<std::string>());
        query.set_contracts_source(daily.at("contracts_source").get<std::string>());
        query.set_exchange("SHFE");
        query.set_product("rb");
        dictionary = data::table_listed_contracts(data->tables(), query);
        open();
    }

    void open()
    {
        notices = std::make_unique<intelligence::Store>(notice_root, notice_owner, clock);
        programs::Files programs(notice_root / "programs", notice_owner);
        const auto executable = current_executable();
        programme = programs.preserve(executable, sha256_file(executable));
        notices->set_programme(programme);
    }

    ip::NoticeCandidate candidate(const std::string& id) const
    {
        ip::NoticeCandidate c;
        c.set_id(id);
        *c.mutable_material() = material.receipt().reference();
        *c.mutable_dictionary() = dictionary.query();
        c.set_contract("SHFE/rb/2026-11");
        const auto quote = [&](ip::EvidenceSpan* span, const std::string& text) {
            const auto pos = body.find(text);
            if (pos == std::string::npos)
                throw std::runtime_error("missing notice fixture quote");
            span->set_begin(pos);
            span->set_end(pos + text.size());
            span->set_text(text);
        };
        quote(c.mutable_subject(), c.contract());
        quote(c.mutable_parameter(), "保证金率");
        quote(c.mutable_effective_day(), "2026-10-24");
        quote(c.mutable_percentage(), "12%");
        quote(c.mutable_modality(), "计划");
        c.set_rule_id("contract_margin_announcement");
        c.set_use_scope("research");
        c.set_use_mode(ip::POST_HOC_RESEARCH);
        return c;
    }

    intelligence::PreparedNotice prepare(const ip::NoticeCandidate& c)
    {
        return intelligence::prepare_notice(notice_owner, c, material, dictionary, programme);
    }

    data::proto::DataRetention hold(const resource::proto::Object& ref) const
    {
        data::proto::DataRetention r;
        r.mutable_source()->set_node_id(data_owner.node_id());
        r.mutable_source()->set_service_id(data_owner.service_id());
        *r.mutable_consumer() = ref;
        auto& v = *r.add_versions();
        v.set_kind(data::proto::DataVersion::TABLE_SNAPSHOT);
        v.set_version(dictionary.query().snapshot());
        return r;
    }

    ip::NoticeRecord admit(const intelligence::PreparedNotice& prepared)
    {
        notices->stage(prepared);
        const auto r = hold(prepared.reference());
        retentions->prepare(r);
        const auto confirmed = retentions->confirm(r);
        return notices->admit(prepared.reference(), confirmed);
    }

    ip::NoticeReview
    review(const resource::proto::Object& ref, const std::string& id = "review-one") const
    {
        ip::NoticeReview r;
        r.set_id(id);
        *r.mutable_candidate() = ref;
        r.set_operator_name("isolated tester");
        r.set_reason("Compared every field with the exact source; this is a planned announcement.");
        r.set_action(ip::NoticeReview::APPROVE);
        for (const auto* name :
            {"subject", "time", "quantity_unit", "modality_context", "source_use", "conflicts"}) {
            auto& c = *r.add_checks();
            c.set_field(name);
            c.set_state(ip::PASS);
        }
        return r;
    }

    ip::NoticePublishRequest publish_request(const resource::proto::Object& ref) const
    {
        ip::NoticePublishRequest r;
        r.set_id("publish-one");
        *r.mutable_candidate() = ref;
        *r.mutable_data_owner() = data_owner;
        r.set_reason("Publish the reviewed source statement for post-hoc research only.");
        return r;
    }

    ip::NoticeStorageReceipt receipt(const ip::NoticePublicationIntent& intent) const
    {
        ip::NoticeStorageReceipt r;
        *r.mutable_origin() = intent.reference();
        auto& c = *r.mutable_content();
        *c.mutable_owner() = data_owner;
        c.set_kind("intelligence_content");
        c.set_id(intent.reference().version());
        c.set_version(c.id());
        r.set_bytes(intent.payload().ByteSizeLong());
        r.set_stored_at_ms(clock->utc_now() / 1000000 + 1);
        return r;
    }
};

} // namespace

TEST(NoticeStore, RealDataRetentionAndExplicitHumanReviewPrecedeStorageAndSemanticPublication)
{
    Fixture f;
    const auto prepared = f.prepare(f.candidate("candidate-one"));
    auto stage = f.notices->stage(prepared);
    ASSERT_TRUE(stage.assessment().ready_for_human_review());
    EXPECT_EQ(stage.state(), ip::PREPARING);
    const auto hold = f.hold(prepared.reference());
    const auto pending = f.retentions->prepare(hold);
    EXPECT_THROW(f.notices->admit(prepared.reference(), pending), Error);
    EXPECT_EQ(f.notices->get("candidate-one").state(), ip::PREPARING);
    f.notices->admit(prepared.reference(), f.retentions->confirm(hold));
    EXPECT_EQ(f.retentions->holds(hold.versions(0)).size(), 1);
    auto human = f.review(prepared.reference());
    human.mutable_checks()->RemoveLast();
    EXPECT_THROW(f.notices->review(human), std::invalid_argument);
    EXPECT_EQ(f.notices->get("candidate-one").state(), ip::DRAFT);
    human = f.review(prepared.reference());
    f.clock->advance(1000000);
    const auto verified = f.notices->review(human);
    EXPECT_EQ(f.notices->get("candidate-one").state(), ip::VERIFIED);
    EXPECT_FALSE(f.notices->get("candidate-one").has_published());
    const auto request = f.publish_request(prepared.reference());
    const auto intent = f.notices->begin_publication(request);
    EXPECT_EQ(f.notices->get("candidate-one").state(), ip::PUBLISHING);
    EXPECT_EQ(
        f.notices->begin_publication(request).SerializeAsString(),
        intent.SerializeAsString());
    EXPECT_EQ(
        f.notices->preparation(intent.reference()).SerializeAsString(),
        intent.SerializeAsString());
    auto premature = intent.reference();
    premature.set_kind("intelligence_publication");
    EXPECT_THROW(f.notices->publication(premature), Error);
    data::IntelligenceContent content(f.data_root, f.data_owner, f.clock);
    unsigned fetches = 0;
    const auto fetch = [&](const resource::proto::Object& origin) {
        ++fetches;
        return f.notices->preparation(origin);
    };
    auto stored = content.save(intent.reference(), fetch);
    EXPECT_EQ(fetches, 1);
    EXPECT_EQ(
        content.read(intent.reference()).payload.SerializeAsString(),
        intent.payload().SerializeAsString());
    const auto offline = [&](const resource::proto::Object&) -> ip::NoticePublicationIntent {
        throw std::runtime_error("source offline");
    };
    EXPECT_EQ(
        content.save(intent.reference(), offline).SerializeAsString(),
        stored.SerializeAsString());
    data::IntelligenceContent restarted(f.data_root, f.data_owner, f.clock);
    EXPECT_EQ(
        restarted.read(intent.reference()).receipt.SerializeAsString(),
        stored.SerializeAsString());
    auto wrong = stored;
    wrong.mutable_content()->mutable_owner()->set_service_id("another-data");
    EXPECT_THROW(f.notices->confirm_publication(prepared.reference(), wrong), Error);
    EXPECT_EQ(f.notices->get("candidate-one").state(), ip::PUBLISHING);
    f.clock->advance(2000000);
    const auto published = f.notices->confirm_publication(prepared.reference(), stored);
    EXPECT_EQ(f.notices->get("candidate-one").state(), ip::PUBLISHED);
    EXPECT_GE(published.publication().available_at_ms(), stored.stored_at_ms());
    EXPECT_EQ(
        f.notices->confirm_publication(prepared.reference(), stored).SerializeAsString(),
        published.SerializeAsString());
    EXPECT_EQ(f.notices->review(human).SerializeAsString(), verified.SerializeAsString());
    auto other = request;
    other.set_id("duplicate-publication");
    EXPECT_THROW(f.notices->begin_publication(other), Error);
    EXPECT_EQ(
        f.notices->release_decision(prepared.reference()).state(),
        resource::proto::ReleaseDecision::RETAINED);
}

TEST(NoticeStore, FailedCommitRollsBackAndOriginalIntentSurvivesRestartWithoutReprocessingSources)
{
    Fixture f;
    const auto prepared = f.prepare(f.candidate("candidate-one"));
    f.admit(prepared);
    const auto review = f.review(prepared.reference());
    f.clock->advance(1000000);
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(f.notices->review(review), std::runtime_error);
    EXPECT_EQ(f.notices->get("candidate-one").state(), ip::DRAFT);
    f.notices->review(review);
    const auto intent = f.notices->begin_publication(f.publish_request(prepared.reference()));
    f.notices.reset();
    f.open();
    EXPECT_EQ(f.notices->get("candidate-one").state(), ip::PUBLISHING);
    EXPECT_EQ(
        f.notices->preparation(intent.reference()).SerializeAsString(),
        intent.SerializeAsString());
    const auto stored = f.receipt(intent);
    f.clock->advance(2000000);
    const auto published = f.notices->confirm_publication(prepared.reference(), stored);
    f.notices.reset();
    f.open();
    EXPECT_EQ(
        f.notices->publication(published.reference()).SerializeAsString(),
        published.SerializeAsString());
    EXPECT_EQ(f.notices->list("", 64).size(), 1);
}

TEST(
    NoticeStore,
    MissingEvidenceCannotBeHumanApprovedAndOnlyUnadmittedPreparationsReleaseReferences)
{
    Fixture f;
    auto c = f.candidate("blocked");
    c.mutable_percentage()->clear_text();
    const auto incomplete = f.prepare(c);
    f.admit(incomplete);
    EXPECT_FALSE(f.notices->get("blocked").assessment().ready_for_human_review());
    EXPECT_THROW(f.notices->review(f.review(incomplete.reference())), Error);
    auto reject = f.review(incomplete.reference());
    reject.set_action(ip::NoticeReview::REJECT);
    reject.clear_checks();
    f.notices->review(reject);
    EXPECT_EQ(f.notices->get("blocked").state(), ip::REJECTED);
    EXPECT_EQ(
        f.notices->release_decision(incomplete.reference()).state(),
        resource::proto::ReleaseDecision::RETAINED);
    const auto staged = f.prepare(f.candidate("unadmitted"));
    f.notices->stage(staged);
    const auto hold = f.hold(staged.reference());
    f.retentions->prepare(hold);
    EXPECT_THROW(
        f.retentions->release(staged.reference(), f.notices->release_decision(staged.reference())),
        Error);
    const auto abandoned = f.notices->abandon(
        staged.reference(),
        "abandon-one",
        "Discard this unadmitted preparation.");
    EXPECT_EQ(abandoned.state(), resource::proto::ReleaseDecision::RELEASED);
    f.retentions->release(staged.reference(), abandoned);
    EXPECT_EQ(
        f.retentions->get(staged.reference()).phase(),
        data::proto::DataRetentionRecord::RELEASED);
    EXPECT_THROW(f.notices->admit(staged.reference(), {}), Error);
    EXPECT_THROW(
        f.notices
            ->abandon(incomplete.reference(), "not-a-draft", "Must not erase formal evidence."),
        Error);
    auto unknown = staged.reference();
    unknown.set_id("unknown");
    EXPECT_EQ(
        f.notices->release_decision(unknown).state(),
        resource::proto::ReleaseDecision::UNKNOWN);
}

TEST(NoticeStore, OwnershipAndGuardLossNeverCreateAnEmptyReplacementLedger)
{
    Fixture f;
    const auto prepared = f.prepare(f.candidate("candidate-one"));
    f.notices->stage(prepared);
    EXPECT_THROW((intelligence::Store(f.notice_root, f.notice_owner, f.clock)), std::runtime_error);
    f.notices.reset();
    auto foreign = f.notice_owner;
    foreign.set_service_id("another-intelligence");
    EXPECT_THROW((intelligence::Store(f.notice_root, foreign, f.clock)), Error);
    fs::remove(f.notice_root / "owner.pb");
    EXPECT_THROW((intelligence::Store(f.notice_root, f.notice_owner, f.clock)), Error);
    EXPECT_FALSE(fs::exists(f.notice_root / "owner.pb"));
    EXPECT_TRUE(fs::is_regular_file(f.notice_root / "ledger.sqlite"));
}

TEST(NoticeStore, DataSavedContentRejectsWrongTargetsAndNeverRepairsLostPublishedEvidence)
{
    Fixture f;
    const auto prepared = f.prepare(f.candidate("candidate-one"));
    f.admit(prepared);
    f.clock->advance(1000000);
    f.notices->review(f.review(prepared.reference()));
    const auto intent = f.notices->begin_publication(f.publish_request(prepared.reference()));
    data::IntelligenceContent content(f.data_root, f.data_owner, f.clock);
    unsigned calls = 0;
    const auto fetch = [&](const resource::proto::Object& origin) {
        ++calls;
        return f.notices->preparation(origin);
    };
    auto other = f.data_owner;
    other.set_service_id("another-data");
    data::IntelligenceContent wrong_target(f.data_root, other, f.clock);
    EXPECT_THROW(wrong_target.save(intent.reference(), fetch), Error);
    EXPECT_FALSE(fs::exists(f.data_root / "intelligence-content.required.pb"));
    auto remote = intent.reference();
    remote.mutable_owner()->set_node_id("remote-node");
    const auto before_remote = calls;
    EXPECT_THROW(content.save(remote, fetch), Error);
    EXPECT_EQ(calls, before_remote);
    const auto receipt = content.save(intent.reference(), fetch);
    EXPECT_EQ(f.notices->get("candidate-one").state(), ip::PUBLISHING);
    EXPECT_THROW((data::IntelligenceContent(f.data_root, other, f.clock)), Error);
    const auto body =
        f.data_root / "intelligence-content" / "objects" / receipt.content().version();
    {
        std::ofstream damaged(body, std::ios::binary | std::ios::trunc);
        damaged << "corrupt";
    }
    const auto before_repeat = calls;
    EXPECT_THROW(content.read(intent.reference()), std::invalid_argument);
    EXPECT_THROW(content.save(intent.reference(), fetch), std::invalid_argument);
    EXPECT_EQ(calls, before_repeat);
    EXPECT_EQ(fs::file_size(body), 7);
    fs::remove(body);
    EXPECT_THROW(content.save(intent.reference(), fetch), std::invalid_argument);
    EXPECT_EQ(calls, before_repeat);
    EXPECT_FALSE(fs::exists(body));
    fs::remove(f.data_root / "intelligence-content.required.pb");
    EXPECT_THROW(
        (data::IntelligenceContent(f.data_root, f.data_owner, f.clock)),
        std::invalid_argument);
}

namespace {

using namespace std::chrono_literals;

struct ResearchNoticeObservation
{
    std::string endpoint, artifact;
    std::atomic<std::uint64_t> pid{0};
};

class NoticeDiscovery
{
    testing_support::LocalListener listener_;
    std::jthread worker_;

public:
    NoticeDiscovery(
        std::string endpoint,
        std::string data_endpoint,
        std::string intel_endpoint,
        std::shared_ptr<ResearchNoticeObservation> research = {})
        : listener_(std::move(endpoint), 8),
          worker_([this,
                      data_endpoint = std::move(data_endpoint),
                      intel_endpoint = std::move(intel_endpoint),
                      research = std::move(research)](std::stop_token stop) {
              while (!stop.stop_requested()) {
                  try {
                      auto peer = listener_.accept(100ms);
                      node::proto::Request request;
                      if (!request.ParseFromString(peer.receive(200ms)))
                          continue;
                      node::proto::Response response;
                      response.set_version(request.version());
                      response.set_correlation_id(request.correlation_id());
                      if (request.has_status()) {
                          auto& status = *response.mutable_status();
                          status.set_node_id("fixture-node");
                          status.set_instance_id("fixture-generation");
                          status.set_phase(node::proto::Status::READY);
                          auto* data = status.add_services();
                          data->set_id("historical-data");
                          data->set_kind(node::proto::DATA);
                          data->set_state("running");
                          data->set_endpoint(data_endpoint);
                          auto* intel = status.add_services();
                          intel->set_id("intelligence-one");
                          intel->set_kind(node::proto::INTELLIGENCE);
                          intel->set_state("running");
                          intel->set_endpoint(intel_endpoint);
                          if (research && research->pid.load()) {
                              auto* actor = status.add_services();
                              actor->set_id("research-one");
                              actor->set_kind(node::proto::RESEARCH);
                              actor->set_state("running");
                              actor->set_endpoint(research->endpoint);
                              actor->set_pid(research->pid.load());
                              actor->set_artifact(research->artifact);
                          }
                      } else {
                          response.mutable_error()->set_code("permission_denied");
                          response.mutable_error()->set_message("read-only discovery fixture");
                      }
                      peer.send(response.SerializeAsString(), 200ms);
                  } catch (const Error&) {
                  }
              }
          })
    {}
};

ip::IntelligenceResponse notice_call(const std::string& endpoint, ip::IntelligenceRequest request)
{
    request.set_version(1);
    request.set_service_id("intelligence-one");
    request.set_correlation_id(next_correlation_id());
    auto channel = ipc::Channel::connect(endpoint, 1s);
    channel.send(request.SerializeAsString(), 1s);
    ip::IntelligenceResponse response;
    if (!response.ParseFromString(channel.receive(15s)))
        throw std::runtime_error("invalid isolated Intelligence response");
    protocol::validate_message(response);
    if (response.version() != request.version() || response.service_id() != request.service_id()
        || response.correlation_id() != request.correlation_id())
        throw std::runtime_error("isolated Intelligence envelope mismatch");
    if (response.has_error())
        throw_remote_error(response.error().code(), response.error().message());
    return response;
}

void stop_notice_process(std::unique_ptr<ChildProcess>& child)
{
    child->request_stop();
    if (!child->wait(12s))
        throw std::runtime_error("isolated service stop timed out");
    EXPECT_EQ(child->exit_code(), 0);
    child.reset();
}

} // namespace

TEST(NoticeProcess, HumanReviewPublishesThroughRealDataSaveAndRestartReplaysOriginalReceipt)
{
    Fixture f;
    const auto candidate = f.candidate("candidate-one");
    auto correction_bytes = f.body;
    correction_bytes.replace(correction_bytes.find("12%"), 3, "13%");
    const std::string withdrawal_bytes = "The corrected announcement is withdrawn.";
    const auto seed_material = [&](const std::string& id, const std::string& body) {
        auto request = f.material.receipt().evidence().request();
        request.set_id(id);
        data::Materials originals(f.data_root, f.data_owner);
        return originals.publish(
            originals.prepare(request, body, "text/plain", f.clock->utc_now() / 1000000));
    };
    const auto correction_material = seed_material("process-correction-original", correction_bytes);
    const auto withdrawal_material = seed_material("process-withdrawal-original", withdrawal_bytes);
    f.notices.reset();
    f.retentions.reset();
    f.data.reset();
    const auto discovery = (f.folder.path / "a.sock").string();
    const auto data_endpoint = (f.folder.path / "d.sock").string();
    const auto intel_endpoint = (f.folder.path / "i.sock").string();
    auto observed_research = std::make_shared<ResearchNoticeObservation>();
    observed_research->endpoint = (f.folder.path / "r.sock").string();
    observed_research->artifact = sha256_file(ASTERION_RESEARCH_SERVICE_PATH);
    NoticeDiscovery agent(discovery, data_endpoint, intel_endpoint, observed_research);
    auto data = std::make_unique<ChildProcess>(
        ASTERION_DATA_SERVICE_PATH,
        std::vector<std::string>{
            "--directory",
            f.data_root.string(),
            "--session",
            "historical-data",
            "--node-id",
            "fixture-node",
            "--endpoint",
            data_endpoint,
            "--discovery-endpoint",
            discovery,
            "--internal-endpoint",
            (f.folder.path / "w.sock").string()
        },
        false,
        f.folder.path / "data.log",
        true);
    protocol::DataClient readiness(data_endpoint, "historical-data");
    const auto data_deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        if (data->exited())
            throw std::runtime_error("isolated Data exited");
        try {
            data::proto::DataRequest heartbeat;
            heartbeat.mutable_heartbeat();
            if (readiness.call(heartbeat, 1s).health().initialized())
                break;
        } catch (const Error& e) {
            if (e.code() != ErrorCode::unavailable)
                throw;
        }
        if (std::chrono::steady_clock::now() >= data_deadline)
            throw std::runtime_error("isolated Data readiness timeout");
        std::this_thread::sleep_for(10ms);
    }
    std::unique_ptr<ChildProcess> intel;
    unsigned starts = 0;
    const auto start = [&] {
        intel = std::make_unique<ChildProcess>(
            ASTERION_INTELLIGENCE_SERVICE_PATH,
            std::vector<std::string>{
                "--directory",
                f.notice_root.string(),
                "--session",
                "intelligence-one",
                "--node-id",
                "fixture-node",
                "--program-artifact",
                sha256_file(ASTERION_INTELLIGENCE_SERVICE_PATH),
                "--endpoint",
                intel_endpoint,
                "--discovery-endpoint",
                discovery
            },
            false,
            f.folder.path / ("intelligence-" + std::to_string(++starts) + ".log"),
            true);
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            if (intel->exited())
                throw std::runtime_error("isolated Intelligence exited");
            try {
                ip::IntelligenceRequest q;
                q.mutable_heartbeat();
                if (notice_call(intel_endpoint, q).health().initialized())
                    break;
            } catch (const Error& e) {
                if (e.code() != ErrorCode::unavailable)
                    throw;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("isolated Intelligence readiness timeout");
            std::this_thread::sleep_for(10ms);
        }
    };
    start();
    ip::IntelligenceRequest q;
    *q.mutable_submit() = candidate;
    const auto record = notice_call(intel_endpoint, q).record();
    ASSERT_EQ(record.state(), ip::DRAFT);
    ASSERT_TRUE(record.assessment().ready_for_human_review());
    q.Clear();
    *q.mutable_review() = f.review(record.reference());
    ASSERT_TRUE(notice_call(intel_endpoint, q).has_verification());
    q.Clear();
    *q.mutable_publish() = f.publish_request(record.reference());
    const auto published = notice_call(intel_endpoint, q).publication();
    protocol::validate_notice_published(published);
    EXPECT_EQ(
        notice_call(intel_endpoint, q).publication().SerializeAsString(),
        published.SerializeAsString());
    protocol::DataClient client(data_endpoint, "historical-data");
    data::proto::DataRequest read;
    *read.mutable_intelligence_read() = published.publication().preparation();
    const auto content = client.call(read, 3s).intelligence_content();
    EXPECT_EQ(
        content.receipt().SerializeAsString(),
        published.publication().storage().SerializeAsString());
    EXPECT_EQ(
        content.payload().candidate().SerializeAsString(),
        record.reference().SerializeAsString());
    ip::IntelligenceRequest delivery_query;
    auto& delivery_filter = *delivery_query.mutable_deliveries()->mutable_filter();
    delivery_filter.add_contracts(candidate.contract());
    delivery_filter.set_rule_id(candidate.rule_id());
    delivery_filter.set_use_scope(candidate.use_scope());
    delivery_filter.set_include_followup_statements(true);
    for (auto kind : {ip::STATEMENT_PUBLISHED, ip::EVENT_CORRECTED, ip::EVENT_WITHDRAWN})
        delivery_filter.add_kinds(kind);
    delivery_query.mutable_deliveries()->set_limit(16);
    const auto initial_deliveries = notice_call(intel_endpoint, delivery_query).deliveries();
    protocol::validate_notice_delivery_page(initial_deliveries);
    EXPECT_EQ(initial_deliveries.head(), 1);
    ASSERT_EQ(initial_deliveries.items_size(), 1);
    EXPECT_EQ(
        initial_deliveries.items(0).source().SerializeAsString(),
        published.reference().SerializeAsString());
    ip::IntelligenceRequest subscription_query;
    auto& subscription_definition = *subscription_query.mutable_subscription()->mutable_create();
    subscription_definition.set_id("process-event-reminders");
    subscription_definition.set_name("Isolated application reminders");
    *subscription_definition.mutable_filter() = delivery_filter;
    subscription_definition.mutable_filter()->set_include_followup_statements(false);
    subscription_definition.set_consumer(ip::NoticeSubscriptionDefinition::APPLICATION_REMINDER);
    auto subscription = notice_call(intel_endpoint, subscription_query).subscription();
    protocol::validate_notice_subscription_view(subscription);
    EXPECT_EQ(subscription.subscription().gap_through(), 1);
    ip::NoticeSubscriptionOffer initial_offer_request;
    *initial_offer_request.mutable_subscription() = subscription.subscription().reference();
    initial_offer_request.set_operation_id("process-initial-history");
    initial_offer_request.set_through(1);
    initial_offer_request.set_historical(true);
    subscription_query.Clear();
    *subscription_query.mutable_subscription()->mutable_offer() = initial_offer_request;
    const auto initial_offer = notice_call(intel_endpoint, subscription_query).delivery_batch();
    protocol::validate_notice_delivery_batch(initial_offer);
    EXPECT_EQ(
        notice_call(intel_endpoint, subscription_query).delivery_batch().SerializeAsString(),
        initial_offer.SerializeAsString());
    subscription_query.Clear();
    *subscription_query.mutable_subscription()->mutable_acknowledge() = initial_offer.reference();
    subscription = notice_call(intel_endpoint, subscription_query).subscription();
    EXPECT_EQ(subscription.subscription().confirmed_through(), 1);
    ip::NoticeSubscriptionControl pause_subscription;
    *pause_subscription.mutable_subscription() = subscription.subscription().reference();
    pause_subscription.set_expected_revision(subscription.subscription().revision());
    pause_subscription.set_active(false);
    subscription_query.Clear();
    *subscription_query.mutable_subscription()->mutable_control() = pause_subscription;
    subscription = notice_call(intel_endpoint, subscription_query).subscription();
    EXPECT_FALSE(subscription.subscription().active());
    ip::IntelligenceRequest set_command;
    set_command.mutable_get()->set_id(candidate.id());
    const auto initial_event = notice_call(intel_endpoint, set_command).record().event();
    ip::NoticeSetRequest set_request;
    set_request.set_id("process-fixed-event-set");
    set_request.set_name("Isolated original event set");
    *set_request.mutable_query()->add_events() = initial_event.reference();
    set_request.mutable_query()->set_use_mode(ip::POST_HOC_RESEARCH);
    set_request.mutable_query()->set_as_of_ms(published.publication().available_at_ms());
    set_request.mutable_query()->set_use_scope("research");
    *set_request.mutable_data_owner() = f.data_owner;
    set_command.Clear();
    *set_command.mutable_event_set()->mutable_preview() = set_request;
    const auto set_preview = notice_call(intel_endpoint, set_command).set_preview();
    ASSERT_EQ(set_preview.payload().members_size(), 1);
    set_request = set_preview.payload().request();
    set_command.Clear();
    *set_command.mutable_event_set()->mutable_publish() = set_request;
    const auto fixed_set = notice_call(intel_endpoint, set_command).event_set();
    ASSERT_TRUE(fixed_set.has_published());
    EXPECT_EQ(
        fixed_set.intent().payload().SerializeAsString(),
        set_preview.payload().SerializeAsString());
    data::proto::DataRequest read_set;
    *read_set.mutable_intelligence_set_read() = fixed_set.intent().reference();
    const auto saved_set = client.call(read_set, 3s).intelligence_set_content();
    EXPECT_EQ(
        saved_set.receipt().SerializeAsString(),
        fixed_set.published().storage().SerializeAsString());
    EXPECT_EQ(
        saved_set.payload().SerializeAsString(),
        fixed_set.intent().payload().SerializeAsString());
    auto strict_set_request = set_request;
    strict_set_request.set_id("process-strict-exclusions");
    strict_set_request.mutable_query()->set_use_mode(ip::STRICT_HISTORY);
    set_command.Clear();
    *set_command.mutable_event_set()->mutable_preview() = strict_set_request;
    const auto strict_set_preview = notice_call(intel_endpoint, set_command).set_preview();
    EXPECT_TRUE(strict_set_preview.payload().members().empty());
    ASSERT_EQ(strict_set_preview.payload().exclusions_size(), 1);
    set_command.Clear();
    *set_command.mutable_event_set()->mutable_publish() = strict_set_request;
    EXPECT_THROW(notice_call(intel_endpoint, set_command), Error);
    const auto research_root = f.folder.path / "research";
    fs::create_directory(research_root);
    const auto policy = f.folder.path / "research.json";
    {
        std::ofstream config(policy);
        config << R"({"task_concurrency":1})";
    }
    std::unique_ptr<ChildProcess> research;
    unsigned research_starts = 0;
    const auto research_call = [&](research::proto::ResearchRequest request) {
        request.set_version(1);
        request.set_service_id("research-one");
        request.set_correlation_id(next_correlation_id());
        auto channel = ipc::Channel::connect(observed_research->endpoint, 1s);
        channel.send(request.SerializeAsString(), 1s);
        task::proto::TaskResponse response;
        if (!response.ParseFromString(channel.receive(15s)))
            throw std::runtime_error("invalid isolated notice Research response");
        protocol::validate_message(response);
        if (response.version() != request.version() || response.service_id() != request.service_id()
            || response.correlation_id() != request.correlation_id())
            throw std::runtime_error("isolated notice Research response identity mismatch");
        if (response.has_error())
            throw_remote_error(response.error().code(), response.error().message());
        return response;
    };
    const auto start_research = [&] {
        research = std::make_unique<ChildProcess>(
            ASTERION_RESEARCH_SERVICE_PATH,
            std::vector<std::string>{
                "--directory",
                research_root.string(),
                "--session",
                "research-one",
                "--config",
                policy.string(),
                "--node-id",
                "fixture-node",
                "--discovery-endpoint",
                discovery,
                "--endpoint",
                observed_research->endpoint
            },
            false,
            f.folder.path / ("research-" + std::to_string(++research_starts) + ".log"),
            true);
        observed_research->pid.store(research->id());
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            if (research->exited())
                throw std::runtime_error("isolated notice Research exited");
            try {
                research::proto::ResearchRequest heartbeat;
                heartbeat.mutable_heartbeat();
                if (research_call(heartbeat).health().initialized())
                    break;
            } catch (const Error& e) {
                if (e.code() != ErrorCode::unavailable)
                    throw;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("isolated notice Research readiness timed out");
            std::this_thread::sleep_for(10ms);
        }
    };
    start_research();
    research::proto::ResearchRequest audit;
    audit.mutable_submit()->set_id("notice-audit-one");
    auto& selection = *audit.mutable_submit()->mutable_notice_review();
    *selection.mutable_source() = published.reference();
    selection.set_as_of_ms(published.publication().available_at_ms());
    selection.set_use_scope("research");
    EXPECT_EQ(research_call(audit).task().kind(), task::proto::NOTICE_REVIEW);
    const auto completed = [&](const std::string& id) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            research::proto::ResearchRequest get;
            get.mutable_get()->set_id(id);
            const auto task = research_call(get).task();
            if (task.state() == task::proto::SUCCEEDED)
                break;
            if (task.state() == task::proto::FAILED)
                throw std::runtime_error(task.error());
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("notice Research execution timed out");
            std::this_thread::sleep_for(10ms);
        }
        research::proto::ResearchRequest result;
        result.mutable_result()->set_id(id);
        return research_call(result);
    };
    research::proto::ResearchRequest set_audit;
    set_audit.mutable_submit()->set_id("event-set-audit");
    *set_audit.mutable_submit()->mutable_event_set_review()->mutable_source() =
        fixed_set.published().reference();
    set_audit.mutable_submit()->mutable_event_set_review()->set_use_scope("research");
    EXPECT_EQ(research_call(set_audit).task().kind(), task::proto::EVENT_SET_REVIEW);
    const auto set_audit_result = completed("event-set-audit");
    ASSERT_TRUE(set_audit_result.has_event_set_review());
    const auto& owned_set = set_audit_result.result_task().event_set_review();
    protocol::validate_event_set_review_result(owned_set, set_audit_result.event_set_review());
    EXPECT_EQ(owned_set.publication().SerializeAsString(), fixed_set.SerializeAsString());
    EXPECT_EQ(set_audit_result.event_set_review().as_of_ms(), set_request.query().as_of_ms());
    ASSERT_EQ(set_audit_result.event_set_review().members_size(), 1);
    EXPECT_FALSE(set_audit_result.event_set_review().members(0).strict_history_eligible());
    const auto set_binding = tasks::notice_consumer_binding(
        protocol::task_execution(
            set_audit_result.result_task(),
            sha256_bytes(owned_set.SerializeAsString())),
        "event-set-audit");
    ip::IntelligenceRequest set_custody;
    *set_custody.mutable_consumer()->mutable_get() = set_binding.consumer();
    const auto set_held = notice_call(intel_endpoint, set_custody).consumer();
    EXPECT_EQ(set_held.phase(), ip::NoticeConsumerRecord::CONFIRMED);
    ip::IntelligenceRequest set_consumers;
    *set_consumers.mutable_consumer()->mutable_list()->mutable_source() =
        fixed_set.published().reference();
    set_consumers.mutable_consumer()->mutable_list()->set_limit(16);
    ASSERT_EQ(notice_call(intel_endpoint, set_consumers).consumer_page().items_size(), 1);
    EXPECT_EQ(set_held.binding().SerializeAsString(), set_binding.SerializeAsString());
    EXPECT_EQ(
        set_binding.source().SerializeAsString(),
        fixed_set.published().reference().SerializeAsString());
    research::proto::ResearchRequest set_reconcile;
    set_reconcile.mutable_reconcile_notice_custody()->set_id("event-set-audit");
    EXPECT_EQ(
        research_call(set_reconcile).notice_custody().SerializeAsString(),
        set_held.SerializeAsString());
    auto changed_set_audit = set_audit;
    changed_set_audit.mutable_submit()->mutable_event_set_review()->set_use_scope("trading");
    EXPECT_THROW(research_call(changed_set_audit), Error);
    set_custody.Clear();
    *set_custody.mutable_consumer()->mutable_release() = set_binding.consumer();
    EXPECT_THROW(notice_call(intel_endpoint, set_custody), Error);
    const auto audit_result = completed("notice-audit-one");
    ASSERT_TRUE(audit_result.has_notice_review());
    EXPECT_EQ(
        audit_result.notice_review().source().SerializeAsString(),
        published.reference().SerializeAsString());
    EXPECT_TRUE(audit_result.notice_review().published_by_cutoff());
    EXPECT_FALSE(audit_result.notice_review().strict_history_eligible());
    const auto binding = tasks::notice_consumer_binding(
        protocol::task_execution(
            audit_result.result_task(),
            sha256_bytes(audit_result.result_task().notice_review().SerializeAsString())),
        "notice-audit-one");
    ip::IntelligenceRequest custody;
    *custody.mutable_consumer()->mutable_get() = binding.consumer();
    const auto held = notice_call(intel_endpoint, custody).consumer();
    EXPECT_EQ(held.binding().SerializeAsString(), binding.SerializeAsString());
    EXPECT_EQ(held.phase(), ip::NoticeConsumerRecord::CONFIRMED);
    ASSERT_TRUE(tasks::read_notice_custody(research_root / "notice-audit-one", binding));
    research::proto::ResearchRequest source_observation;
    source_observation.mutable_notice_source_changes()->set_id("notice-audit-one");
    const auto initial_observation = research_call(source_observation).notice_source_observation();
    protocol::validate_notice_source_observation(initial_observation);
    EXPECT_EQ(
        initial_observation.consumer().SerializeAsString(),
        binding.consumer().SerializeAsString());
    EXPECT_FALSE(initial_observation.current().has_latest_change());
    EXPECT_EQ(
        initial_observation.current().selected_publication().SerializeAsString(),
        published.reference().SerializeAsString());
    EXPECT_EQ(
        tasks::read_notice_custody(research_root / "notice-audit-one", binding)
            ->SerializeAsString(),
        held.SerializeAsString());
    research::proto::ResearchRequest reconcile;
    reconcile.mutable_reconcile_notice_custody()->set_id("notice-audit-one");
    EXPECT_EQ(
        research_call(reconcile).notice_custody().SerializeAsString(),
        held.SerializeAsString());
    custody.Clear();
    *custody.mutable_consumer()->mutable_release() = binding.consumer();
    EXPECT_THROW(notice_call(intel_endpoint, custody), Error);
    auto second = audit;
    second.mutable_submit()->set_id("notice-audit-two");
    research_call(second);
    completed("notice-audit-two");
    EXPECT_TRUE(
        fs::equivalent(
            research_root / "notice-audit-one/input.pb",
            research_root / "notice-audit-two/input.pb"));
    auto second_binding = binding;
    second_binding.mutable_consumer()->set_id("notice-audit-two");
    research::proto::ResearchRequest retire;
    retire.mutable_retirement()->set_id("notice-audit-two");
    retire.mutable_retirement()->set_execute(true);
    EXPECT_EQ(research_call(retire).retirement().state(), task::proto::TaskRetirement::RETIRED);
    custody.Clear();
    *custody.mutable_consumer()->mutable_release() = second_binding.consumer();
    EXPECT_THROW(notice_call(intel_endpoint, custody), Error);
    stop_notice_process(research);
    observed_research->pid.store(0);
    // A real pending submission owns its intent before the source hold. Its payload
    // may later be cleaned, but the durable Research decision must still drive release.
    ip::NoticeConsumerBinding pending_binding;
    {
        tasks::Store writer(research_root, {"research-one"});
        auto pending =
            task_submission(writer, "notice-pending", audit_result.result_task().notice_review());
        writer.admit_submission(pending);
        pending.prepare_files();
        pending_binding = tasks::notice_consumer_binding(pending.execution(), "notice-pending");
        custody.Clear();
        *custody.mutable_consumer()->mutable_prepare() = pending_binding;
        const auto prepared = notice_call(intel_endpoint, custody).consumer();
        tasks::save_notice_custody(research_root / "notice-pending", prepared);
        EXPECT_EQ(prepared.phase(), ip::NoticeConsumerRecord::PREPARED);
    }
    start_research();
    research::proto::ResearchRequest abandon;
    abandon.mutable_abandon_preparation()->set_id("notice-pending");
    EXPECT_EQ(
        research_call(abandon).release_decision().state(),
        resource::proto::ReleaseDecision::RELEASED);
    custody.Clear();
    *custody.mutable_consumer()->mutable_get() = pending_binding.consumer();
    const auto released = notice_call(intel_endpoint, custody).consumer();
    EXPECT_EQ(released.phase(), ip::NoticeConsumerRecord::RELEASED);
    stop_notice_process(research);
    observed_research->pid.store(0);
    fs::remove_all(research_root / "notice-pending"); // Only this isolated synthetic preparation.
    start_research();
    EXPECT_EQ(
        research_call(abandon).release_decision().state(),
        resource::proto::ReleaseDecision::RELEASED);
    EXPECT_FALSE(fs::exists(research_root / "notice-pending"));
    EXPECT_EQ(
        notice_call(intel_endpoint, custody).consumer().SerializeAsString(),
        released.SerializeAsString());
    stop_notice_process(research);
    observed_research->pid.store(0);
    custody.Clear();
    *custody.mutable_consumer()->mutable_release() = binding.consumer();
    EXPECT_THROW(notice_call(intel_endpoint, custody), Error);
    custody.Clear();
    *custody.mutable_consumer()->mutable_get() = binding.consumer();
    EXPECT_EQ(
        notice_call(intel_endpoint, custody).consumer().phase(),
        ip::NoticeConsumerRecord::CONFIRMED);
    stop_notice_process(intel);
    data::proto::DataRequest repeat;
    *repeat.mutable_intelligence_save() = published.publication().preparation();
    EXPECT_EQ(
        client.call(repeat, 3s).intelligence_saved().SerializeAsString(),
        content.receipt().SerializeAsString());
    data::proto::DataRequest repeat_set;
    *repeat_set.mutable_intelligence_set_save() = fixed_set.intent().reference();
    EXPECT_EQ(
        client.call(repeat_set, 3s).intelligence_saved().SerializeAsString(),
        saved_set.receipt().SerializeAsString());
    start();
    custody.Clear();
    *custody.mutable_consumer()->mutable_get() = binding.consumer();
    EXPECT_EQ(
        notice_call(intel_endpoint, custody).consumer().SerializeAsString(),
        held.SerializeAsString());
    ip::IntelligenceRequest event_request;
    event_request.mutable_get()->set_id(candidate.id());
    const auto event = notice_call(intel_endpoint, event_request).record().event();
    auto replacement_candidate = candidate;
    replacement_candidate.set_id("process-replacement");
    *replacement_candidate.mutable_material() = correction_material.reference();
    replacement_candidate.mutable_percentage()->set_text("13%");
    *replacement_candidate.mutable_event() = event.reference();
    event_request.Clear();
    *event_request.mutable_submit() = replacement_candidate;
    const auto replacement_record = notice_call(intel_endpoint, event_request).record();
    event_request.Clear();
    *event_request.mutable_review() =
        f.review(replacement_record.reference(), "process-replacement-review");
    ASSERT_TRUE(notice_call(intel_endpoint, event_request).has_verification());
    event_request.Clear();
    auto replacement_publish = f.publish_request(replacement_record.reference());
    replacement_publish.set_id("process-replacement-publish");
    *event_request.mutable_publish() = replacement_publish;
    const auto replacement_publication = notice_call(intel_endpoint, event_request).publication();
    ip::IntelligenceRequest change_command;
    auto& change_request = *change_command.mutable_change();
    change_request.set_id("process-correction");
    *change_request.mutable_event() = event.reference();
    *change_request.mutable_expected_head() = published.reference();
    change_request.set_action(ip::NoticeChangeRequest::CORRECT);
    *change_request.mutable_replacement() = replacement_publication.reference();
    *change_request.mutable_material() = correction_material.reference();
    change_request.mutable_evidence()->set_begin(0);
    change_request.mutable_evidence()->set_end(correction_bytes.size());
    change_request.mutable_evidence()->set_text(correction_bytes);
    change_request.set_operator_name("isolated human");
    change_request.set_reason("Compared the new original: margin rate is corrected to 13%.");
    change_request.set_human_confirmed(true);
    const auto corrected = notice_call(intel_endpoint, change_command).change();
    ASSERT_TRUE(corrected.has_published());
    data::proto::DataRequest change_read;
    *change_read.mutable_intelligence_change_read() = corrected.intent().reference();
    const auto saved_change = client.call(change_read, 3s).intelligence_change_content();
    EXPECT_EQ(
        saved_change.receipt().SerializeAsString(),
        corrected.published().storage().SerializeAsString());
    EXPECT_EQ(
        saved_change.payload().SerializeAsString(),
        corrected.intent().payload().SerializeAsString());
    event_request.Clear();
    *event_request.mutable_event_view()->mutable_event() = event.reference();
    EXPECT_EQ(
        notice_call(intel_endpoint, event_request)
            .event_view()
            .selected_publication()
            .SerializeAsString(),
        replacement_publication.reference().SerializeAsString());
    event_request.mutable_event_view()->set_as_of_ms(published.publication().available_at_ms());
    EXPECT_EQ(
        notice_call(intel_endpoint, event_request)
            .event_view()
            .selected_publication()
            .SerializeAsString(),
        published.reference().SerializeAsString());
    change_request.set_id("process-withdrawal");
    change_request.set_action(ip::NoticeChangeRequest::WITHDRAW);
    change_request.clear_replacement();
    *change_request.mutable_expected_head() = corrected.published().reference();
    *change_request.mutable_material() = withdrawal_material.reference();
    change_request.mutable_evidence()->set_end(withdrawal_bytes.size());
    change_request.mutable_evidence()->set_text(withdrawal_bytes);
    change_request.set_reason("The new original explicitly withdraws the announcement.");
    const auto withdrawn_change = notice_call(intel_endpoint, change_command).change();
    ASSERT_TRUE(withdrawn_change.has_published());
    event_request.mutable_event_view()->clear_as_of_ms();
    EXPECT_TRUE(notice_call(intel_endpoint, event_request).event_view().withdrawn());
    q.Clear();
    *q.mutable_publish() = f.publish_request(record.reference());
    data::proto::DataRequest release;
    *release.mutable_retention()->mutable_release() = record.reference();
    EXPECT_THROW(client.call(release, 3s), Error);
    release.Clear();
    *release.mutable_retention()->mutable_get() = record.reference();
    EXPECT_EQ(
        client.call(release, 3s).retention().phase(),
        data::proto::DataRetentionRecord::CONFIRMED);
    stop_notice_process(data);
    set_command.Clear();
    *set_command.mutable_event_set()->mutable_publish() = set_request;
    EXPECT_EQ(
        notice_call(intel_endpoint, set_command).event_set().SerializeAsString(),
        fixed_set.SerializeAsString());
    EXPECT_EQ(
        notice_call(intel_endpoint, change_command).change().SerializeAsString(),
        withdrawn_change.SerializeAsString());
    EXPECT_EQ(
        notice_call(intel_endpoint, q).publication().SerializeAsString(),
        published.SerializeAsString());
    q.Clear();
    q.mutable_get()->set_id(candidate.id());
    EXPECT_EQ(notice_call(intel_endpoint, q).record().state(), ip::PUBLISHED);
    const auto offline_deliveries = notice_call(intel_endpoint, delivery_query).deliveries();
    protocol::validate_notice_delivery_page(offline_deliveries);
    EXPECT_EQ(offline_deliveries.head(), 4);
    ASSERT_EQ(offline_deliveries.items_size(), 4);
    EXPECT_EQ(
        offline_deliveries.items(2).source().SerializeAsString(),
        corrected.published().reference().SerializeAsString());
    EXPECT_EQ(
        offline_deliveries.items(3).source().SerializeAsString(),
        withdrawn_change.published().reference().SerializeAsString());
    EXPECT_EQ(offline_deliveries.items(3).kind(), ip::EVENT_WITHDRAWN);
    start_research();
    const auto changed_observation = research_call(source_observation);
    ASSERT_TRUE(changed_observation.has_notice_source_observation());
    EXPECT_FALSE(changed_observation.has_result_task());
    EXPECT_TRUE(changed_observation.notice_source_observation().current().withdrawn());
    EXPECT_EQ(
        changed_observation.notice_source_observation().consumer().SerializeAsString(),
        binding.consumer().SerializeAsString());
    EXPECT_EQ(
        changed_observation.notice_source_observation()
            .source()
            .published()
            .reference()
            .SerializeAsString(),
        published.reference().SerializeAsString());
    subscription_query.Clear();
    subscription_query.mutable_subscription()->mutable_get()->set_id("process-event-reminders");
    subscription = notice_call(intel_endpoint, subscription_query).subscription();
    EXPECT_EQ(subscription.head(), 4);
    EXPECT_FALSE(subscription.subscription().active());
    EXPECT_EQ(subscription.subscription().confirmed_through(), 1);
    ip::NoticeReminderQuery reminder_query;
    *reminder_query.mutable_subscription() = subscription.subscription().reference();
    reminder_query.set_limit(16);
    subscription_query.Clear();
    *subscription_query.mutable_subscription()->mutable_reminders() = reminder_query;
    ASSERT_EQ(notice_call(intel_endpoint, subscription_query).reminders().items_size(), 1);
    auto resume_subscription = pause_subscription;
    resume_subscription.set_expected_revision(subscription.subscription().revision());
    resume_subscription.set_active(true);
    subscription_query.Clear();
    *subscription_query.mutable_subscription()->mutable_control() = resume_subscription;
    subscription = notice_call(intel_endpoint, subscription_query).subscription();
    EXPECT_EQ(subscription.subscription().gap_through(), 4);
    auto backfill_request = initial_offer_request;
    backfill_request.set_operation_id("process-resume-history");
    backfill_request.set_after(1);
    backfill_request.set_through(4);
    backfill_request.set_historical(false);
    subscription_query.Clear();
    *subscription_query.mutable_subscription()->mutable_offer() = backfill_request;
    EXPECT_THROW(notice_call(intel_endpoint, subscription_query), Error);
    backfill_request.set_historical(true);
    *subscription_query.mutable_subscription()->mutable_offer() = backfill_request;
    const auto pending_history = notice_call(intel_endpoint, subscription_query).delivery_batch();
    ASSERT_EQ(
        pending_history.payload().page().items_size(),
        2); // C/W, without followup statement R.
    EXPECT_EQ(pending_history.payload().page().items(0).kind(), ip::EVENT_CORRECTED);
    EXPECT_EQ(pending_history.payload().page().items(1).kind(), ip::EVENT_WITHDRAWN);
    stop_notice_process(intel);
    start();
    subscription_query.Clear();
    *subscription_query.mutable_subscription()->mutable_delivery()->mutable_subscription() =
        pending_history.payload().request().subscription();
    subscription_query.mutable_subscription()->mutable_delivery()->set_operation_id(
        pending_history.reference().id());
    EXPECT_EQ(
        notice_call(intel_endpoint, subscription_query).delivery_batch().SerializeAsString(),
        pending_history.SerializeAsString());
    subscription_query.Clear();
    *subscription_query.mutable_subscription()->mutable_acknowledge() = pending_history.reference();
    subscription = notice_call(intel_endpoint, subscription_query).subscription();
    EXPECT_EQ(subscription.subscription().confirmed_through(), 4);
    EXPECT_EQ(subscription.subscription().gap_through(), 0);
    stop_notice_process(intel);
    start(); // The first confirmation is committed; its caller may have lost the reply.
    EXPECT_EQ(
        notice_call(intel_endpoint, subscription_query).subscription().SerializeAsString(),
        subscription.SerializeAsString());
    subscription_query.Clear();
    *subscription_query.mutable_subscription()->mutable_reminders() = reminder_query;
    const auto delivered_reminders = notice_call(intel_endpoint, subscription_query).reminders();
    ASSERT_EQ(delivered_reminders.items_size(), 3);
    EXPECT_TRUE(delivered_reminders.items(1).historical());
    EXPECT_TRUE(delivered_reminders.items(2).historical());
    stop_notice_process(intel);
    EXPECT_THROW(research_call(source_observation), Error); // Unknown never becomes "no changes".
    EXPECT_EQ(research_call(audit).task().state(), task::proto::SUCCEEDED);
    EXPECT_EQ(research_call(set_audit).task().state(), task::proto::SUCCEEDED);
    const auto restored_set_audit = completed("event-set-audit");
    EXPECT_EQ(
        restored_set_audit.event_set_review().SerializeAsString(),
        set_audit_result.event_set_review().SerializeAsString());
    EXPECT_EQ(
        restored_set_audit.result_task().result_digest(),
        set_audit_result.result_task().result_digest());
    const auto restored_audit = completed("notice-audit-one");
    EXPECT_EQ(
        restored_audit.notice_review().SerializeAsString(),
        audit_result.notice_review().SerializeAsString());
    EXPECT_EQ(
        restored_audit.result_task().result_digest(),
        audit_result.result_task().result_digest());
    stop_notice_process(research);
    observed_research->pid.store(0);
}

TEST(NoticeReviewContract, FixesPublishedSemanticProofAndExactDataBytesWithoutWideningSourceUse)
{
    Fixture f;
    const auto prepared = f.prepare(f.candidate("candidate-one"));
    f.admit(prepared);
    f.clock->advance(1000000);
    f.notices->review(f.review(prepared.reference()));
    const auto intent = f.notices->begin_publication(f.publish_request(prepared.reference()));
    data::IntelligenceContent data(f.data_root, f.data_owner, f.clock);
    const auto receipt = data.save(intent.reference(), [&](const auto& source) {
        return f.notices->preparation(source);
    });
    f.clock->advance(1000000);
    const auto publication = f.notices->confirm_publication(prepared.reference(), receipt);
    const auto read = data.read(intent.reference());
    ip::NoticeStoredContent content;
    *content.mutable_receipt() = read.receipt;
    *content.mutable_payload() = read.payload;
    research::proto::NoticeReviewRequest request;
    *request.mutable_source() = publication.reference();
    request.set_as_of_ms(publication.publication().available_at_ms() - 1);
    request.set_use_scope("research");
    const auto input = protocol::make_notice_review_input(
        owner("research", "research-one"),
        request,
        publication,
        content,
        f.programme.digest);
    EXPECT_EQ(
        protocol::notice_review_execution(input).request().SerializeAsString(),
        request.SerializeAsString());
    EXPECT_EQ(
        protocol::decode_notice_review_input(input).at("source").at("version"),
        publication.reference().version());
    auto tampered = input;
    tampered.mutable_content()
        ->mutable_payload()
        ->mutable_assessment()
        ->mutable_margin_ratio()
        ->set_units(1);
    EXPECT_THROW(protocol::validate_notice_review_input(tampered), std::invalid_argument);
    tampered = input;
    tampered.mutable_request()->set_use_scope("another-use");
    EXPECT_THROW(protocol::validate_notice_review_input(tampered), std::invalid_argument);
    tampered = input;
    tampered.mutable_request()->mutable_source()->set_kind("intelligence_preparation");
    EXPECT_THROW(protocol::validate_notice_review_input(tampered), std::invalid_argument);
    tampered = input;
    tampered.mutable_content()
        ->mutable_receipt()
        ->mutable_content()
        ->mutable_owner()
        ->set_service_id("another-data");
    EXPECT_THROW(protocol::validate_notice_review_input(tampered), std::invalid_argument);
}

namespace {

research::proto::NoticeReviewInput published_notice_input(Fixture& f)
{
    const auto prepared = f.prepare(f.candidate("candidate-one"));
    f.admit(prepared);
    f.clock->advance(1000000);
    f.notices->review(f.review(prepared.reference()));
    const auto intent = f.notices->begin_publication(f.publish_request(prepared.reference()));
    data::IntelligenceContent data(f.data_root, f.data_owner, f.clock);
    const auto receipt = data.save(intent.reference(), [&](const auto& source) {
        return f.notices->preparation(source);
    });
    f.clock->advance(1000000);
    const auto publication = f.notices->confirm_publication(prepared.reference(), receipt);
    const auto read = data.read(intent.reference());
    ip::NoticeStoredContent content;
    *content.mutable_receipt() = read.receipt;
    *content.mutable_payload() = read.payload;
    research::proto::NoticeReviewRequest request;
    *request.mutable_source() = publication.reference();
    request.set_as_of_ms(publication.publication().available_at_ms());
    request.set_use_scope("research");
    return protocol::make_notice_review_input(
        owner("research", "research-one"),
        request,
        publication,
        content,
        f.programme.digest);
}

task::proto::TaskAttempt notice_attempt(tasks::Store& store, const std::string& id)
{
    auto read = store.prepare_input(id);
    read.load_for_claim();
    const auto token = store.commit(store.claim(read)).token();
    auto attempt = store.confirm_attempt(std::move(read));
    attempt.set_token(token);
    EXPECT_TRUE(attempt.task().has_notice_review());
    EXPECT_TRUE(attempt.execution().has_notice_review());
    EXPECT_FALSE(attempt.execution().has_data_owner());
    attempt.set_data_endpoint("/unavailable-audit-source.sock");
    attempt.set_data_instance("must-not-be-read");
    return attempt;
}

} // namespace

TEST(NoticeReviewStore, ReusesFixedInputAndRetainsExactResultsAcrossRestartWithoutSourceReads)
{
    Fixture f;
    const auto input = published_notice_input(f);
    const auto root = f.folder.path / "research";
    fs::create_directory(root);
    std::string digest;
    {
        tasks::Store store(root, {"research-one"});
        tasks::submit(store, "audit-one", input);
        tasks::submit(store, "audit-two", input);
        EXPECT_TRUE(fs::equivalent(root / "audit-one/input.pb", root / "audit-two/input.pb"));
        auto attempt = notice_attempt(store, "audit-one");
        unsigned completed = 0;
        auto output = tasks::execute(
            attempt,
            root,
            {},
            [&](auto done, auto total) {
                EXPECT_EQ(total, 1U);
                completed = done;
            },
            input.program_artifact());
        EXPECT_EQ(completed, 1U);
        protocol::validate_notice_review_result(input, output.notice_review());
        EXPECT_TRUE(output.notice_review().published_by_cutoff());
        EXPECT_FALSE(output.notice_review().strict_history_eligible());
        EXPECT_THROW(
            (void)tasks::execute(
                attempt,
                root,
                {},
                [](auto, auto) {},
                std::string(64, 'f')),
            Error);
        store.commit(tasks::finish(store, std::move(output)));
        const auto response = tasks::verified_result(store, "audit-one");
        const auto decoded = protocol::decode_task_result(response, "audit-one");
        EXPECT_EQ(decoded.at("kind"), "notice_review");
        EXPECT_EQ(
            decoded.at("result").at("source").at("version"),
            input.request().source().version());
        EXPECT_FALSE(store.describe("audit-one").has_notice_review());
        digest = store.describe("audit-one").result_digest();
        resource::proto::CatalogueQuery q;
        q.set_kind("notice_review_result");
        q.set_limit(20);
        ASSERT_EQ(store.resources("fixture-node", q).items_size(), 1);
    }
    {
        tasks::Store recovered(root, {"research-one"});
        const auto result = tasks::verified_result(recovered, "audit-one");
        EXPECT_EQ(result.result_task().result_digest(), digest);
        EXPECT_EQ(
            result.result_task().notice_review().SerializeAsString(),
            input.SerializeAsString());
        EXPECT_EQ(
            result.notice_review().source().SerializeAsString(),
            input.request().source().SerializeAsString());
    }
}

namespace {

ip::NoticeConsumerBinding
notice_binding(const research::proto::NoticeReviewInput& input, const std::string& id)
{
    ip::NoticeConsumerBinding binding;
    *binding.mutable_source() = input.request().source();
    auto& consumer = *binding.mutable_consumer();
    *consumer.mutable_owner() = input.owner();
    consumer.set_kind("notice_review");
    consumer.set_id(id);
    consumer.set_version(sha256_bytes(input.SerializeAsString()));
    binding.set_use_scope(input.request().use_scope());
    return binding;
}

} // namespace

TEST(NoticeSubscriptions, MissingOwnerGuardDoesNotRecreateAnApparentlyEmptySource)
{
    Fixture f;
    ip::NoticeSubscriptionDefinition definition;
    definition.set_id("subscription-without-publications");
    definition.set_name("Durable conditions before first publication");
    definition.set_consumer(ip::NoticeSubscriptionDefinition::APPLICATION_REMINDER);
    definition.mutable_filter()->add_contracts(f.candidate("query-only").contract());
    definition.mutable_filter()->set_rule_id("contract_margin_announcement");
    definition.mutable_filter()->set_use_scope("research");
    definition.mutable_filter()->add_kinds(ip::STATEMENT_PUBLISHED);
    EXPECT_EQ(f.notices->subscriptions().items_size(), 0);
    const auto saved = f.notices->create_subscription(definition);
    EXPECT_EQ(saved.head(), 0);
    const auto listing = f.notices->subscriptions();
    ASSERT_EQ(listing.items_size(), 1);
    EXPECT_EQ(listing.owner().SerializeAsString(), f.notice_owner.SerializeAsString());
    EXPECT_EQ(listing.items(0).SerializeAsString(), saved.subscription().SerializeAsString());
    f.notices.reset();
    const auto held = f.folder.path / "held-subscription-owner.pb";
    fs::rename(f.notice_root / "owner.pb", held);
    EXPECT_THROW(f.open(), Error);
    EXPECT_FALSE(fs::exists(f.notice_root / "owner.pb"));
    fs::rename(held, f.notice_root / "owner.pb");
    f.open();
    EXPECT_EQ(
        f.notices->subscription(definition.id()).SerializeAsString(),
        saved.SerializeAsString());
    EXPECT_EQ(f.notices->subscriptions().SerializeAsString(), listing.SerializeAsString());
}

TEST(NoticeConsumers, FixedCustodyIsDurableIdempotentAndDoesNotClaimResearchAdmission)
{
    Fixture f;
    const auto input = published_notice_input(f);
    const auto binding = notice_binding(input, "consumer-one");
    EXPECT_THROW(f.notices->consumer(binding.consumer()), Error);
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(f.notices->prepare_consumer(binding), std::runtime_error);
    EXPECT_THROW(f.notices->consumer(binding.consumer()), Error);
    const auto prepared = f.notices->prepare_consumer(binding);
    EXPECT_EQ(prepared.phase(), ip::NoticeConsumerRecord::PREPARED);
    EXPECT_EQ(
        f.notices->prepare_consumer(binding).SerializeAsString(),
        prepared.SerializeAsString());
    auto changed = binding;
    changed.set_use_scope("another-purpose");
    EXPECT_THROW(f.notices->prepare_consumer(changed), Error);
    changed = binding;
    changed.mutable_consumer()->set_version(std::string(64, 'f'));
    EXPECT_THROW(f.notices->prepare_consumer(changed), Error);
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(f.notices->confirm_consumer(binding), std::runtime_error);
    EXPECT_EQ(f.notices->consumer(binding.consumer()).phase(), ip::NoticeConsumerRecord::PREPARED);
    const auto confirmed = f.notices->confirm_consumer(binding);
    EXPECT_EQ(confirmed.phase(), ip::NoticeConsumerRecord::CONFIRMED);
    EXPECT_EQ(
        f.notices->confirm_consumer(binding).SerializeAsString(),
        confirmed.SerializeAsString());
    f.notices.reset();
    f.open();
    EXPECT_EQ(
        f.notices->consumer(binding.consumer()).SerializeAsString(),
        confirmed.SerializeAsString());
}

TEST(NoticeConsumers, OnlyExactDurableReleaseFreesCustodyAndTheReleasedIdentityCannotRevive)
{
    Fixture f;
    const auto input = published_notice_input(f);
    const auto binding = notice_binding(input, "consumer-release");
    f.notices->prepare_consumer(binding);
    resource::proto::ReleaseDecision proof;
    *proof.mutable_object() = binding.consumer();
    proof.set_state(resource::proto::ReleaseDecision::UNKNOWN);
    EXPECT_THROW(f.notices->release_consumer(binding.consumer(), proof), Error);
    proof.set_state(resource::proto::ReleaseDecision::RETAINED);
    EXPECT_THROW(f.notices->release_consumer(binding.consumer(), proof), Error);
    proof.set_state(resource::proto::ReleaseDecision::RELEASED);
    proof.mutable_object()->set_version(std::string(64, 'f'));
    EXPECT_THROW(f.notices->release_consumer(binding.consumer(), proof), Error);
    *proof.mutable_object() = binding.consumer();
    const auto released = f.notices->release_consumer(binding.consumer(), proof);
    EXPECT_EQ(released.phase(), ip::NoticeConsumerRecord::RELEASED);
    EXPECT_EQ(
        f.notices->release_consumer(binding.consumer(), proof).SerializeAsString(),
        released.SerializeAsString());
    EXPECT_THROW(f.notices->prepare_consumer(binding), Error);
    EXPECT_THROW(f.notices->confirm_consumer(binding), Error);
    f.notices.reset();
    f.open();
    EXPECT_EQ(f.notices->consumer(binding.consumer()).phase(), ip::NoticeConsumerRecord::RELEASED);
}

TEST(NoticeConsumers, ExactOwnerPagesAndMissingAuthorityNeverBecomeZeroReferences)
{
    Fixture f;
    const auto input = published_notice_input(f);
    const auto one = notice_binding(input, "same-id");
    auto two = one;
    two.mutable_consumer()->mutable_owner()->set_service_id("another-research");
    f.notices->prepare_consumer(one);
    f.notices->prepare_consumer(two);
    ip::NoticeConsumerQuery query;
    *query.mutable_source() = input.request().source();
    query.set_limit(1);
    const auto first = f.notices->consumers(query);
    ASSERT_EQ(first.items_size(), 1);
    EXPECT_FALSE(first.next_after().empty());
    query.set_after(first.next_after());
    const auto second = f.notices->consumers(query);
    ASSERT_EQ(second.items_size(), 1);
    EXPECT_TRUE(second.next_after().empty());
    EXPECT_NE(
        first.items(0).binding().consumer().owner().service_id(),
        second.items(0).binding().consumer().owner().service_id());
    query.clear_after();
    query.mutable_source()->set_version(std::string(64, 'f'));
    EXPECT_THROW(f.notices->consumers(query), Error);
    f.notices.reset();
    EXPECT_NO_THROW(intelligence::cold_backup(f.notice_root, f.notice_owner));
    {
        sqlite::Database db(f.notice_root / "ledger.sqlite");
        db.execute("DROP TABLE notice_consumers");
    }
    const auto before = sha256_file(f.notice_root / "ledger.sqlite");
    EXPECT_THROW((intelligence::Store(f.notice_root, f.notice_owner)), Error);
    EXPECT_THROW(intelligence::cold_backup(f.notice_root, f.notice_owner), Error);
    EXPECT_EQ(sha256_file(f.notice_root / "ledger.sqlite"), before);
}

TEST(NoticeResources, SeparatesCandidatesPreparationsPublishedFactsAndSavedBytesWithoutMutation)
{
    Fixture f;
    const auto input = published_notice_input(f);
    const auto pending = f.prepare(f.candidate("unpublished-candidate"));
    (void)f.notices->stage(pending);
    const auto guard_before = sha256_file(f.data_root / "intelligence-content.required.pb");
    resource::proto::CatalogueQuery query;
    query.set_kind("intelligence_candidate");
    query.set_limit(1);
    const auto first = f.notices->resources(query);
    ASSERT_EQ(first.items_size(), 1);
    EXPECT_FALSE(first.next_after().empty());
    query.set_after(first.next_after());
    const auto second = f.notices->resources(query);
    ASSERT_EQ(second.items_size(), 1);
    EXPECT_NE(first.items(0).object().id(), second.items(0).object().id());
    EXPECT_TRUE(second.next_after().empty());
    query.clear_after();
    query.set_kind("intelligence_publication");
    const auto published = f.notices->resources(query);
    ASSERT_EQ(published.items_size(), 1);
    EXPECT_EQ(
        published.items(0).object().SerializeAsString(),
        input.request().source().SerializeAsString());
    query.set_kind("intelligence_preparation");
    EXPECT_EQ(f.notices->resources(query).items_size(), 1);
    query.set_kind("intelligence_content");
    const auto saved = f.data->resources(f.data_owner, query, {});
    ASSERT_EQ(saved.items_size(), 1);
    EXPECT_EQ(
        saved.items(0).object().SerializeAsString(),
        input.content().receipt().content().SerializeAsString());
    EXPECT_EQ(saved.items(0).logical_bytes(), input.content().receipt().bytes());
    EXPECT_EQ(sha256_file(f.data_root / "intelligence-content.required.pb"), guard_before);
    // A directory read must not scan/hash all payload bytes; it is not an integrity claim.
    auto damaged = input.content().payload().SerializeAsString();
    ASSERT_FALSE(damaged.empty());
    damaged.back() ^= 1;
    {
        std::ofstream body(
            f.data_root / "intelligence-content/objects"
                / input.content().receipt().content().version(),
            std::ios::binary | std::ios::trunc);
        body << damaged;
    }
    EXPECT_EQ(f.data->resources(f.data_owner, query, {}).items_size(), 1);
    const data::IntelligenceContent content(f.data_root, f.data_owner);
    EXPECT_THROW(content.read(input.content().receipt().origin()), std::invalid_argument);
    query.set_kind("intelligence_program");
    EXPECT_EQ(f.notices->resources(query).items_size(), 1);
    query.set_kind("unsupported");
    EXPECT_THROW(f.notices->resources(query), Error);
}

TEST(NoticeJointBackup, ExactThreeDomainCutClosesSavedContentAndSemanticEvidenceWithoutSources)
{
    Fixture f;
    const auto input = published_notice_input(f);
    const auto root = f.folder.path / "joint-research";
    fs::create_directory(root);
    ip::NoticeSetRequest selected_set;
    selected_set.set_id("joint-event-set");
    selected_set.set_name("Explicit joint event selection");
    *selected_set.mutable_data_owner() = f.data_owner;
    auto& query = *selected_set.mutable_query();
    *query.add_events() = f.notices->get("candidate-one").event().reference();
    query.set_as_of_ms(input.request().as_of_ms());
    query.set_use_scope(input.request().use_scope());
    query.set_use_mode(ip::POST_HOC_RESEARCH);
    selected_set = f.notices->preview_set(selected_set).payload().request();
    const auto set_intent = f.notices->begin_set(selected_set);
    data::IntelligenceContent contents(f.data_root, f.data_owner, f.clock);
    const auto set_receipt = contents.save_set(set_intent.reference(), [&](const auto&) {
        return set_intent;
    });
    const auto set_publication = f.notices->confirm_set(set_intent.reference(), set_receipt);
    research::proto::EventSetReviewInput collection;
    *collection.mutable_owner() = input.owner();
    *collection.mutable_request()->mutable_source() = set_publication.published().reference();
    collection.mutable_request()->set_use_scope(input.request().use_scope());
    *collection.mutable_publication() = set_publication;
    *collection.mutable_content()->mutable_receipt() = set_receipt;
    *collection.mutable_content()->mutable_payload() = set_intent.payload();
    *collection.add_members() = input;
    collection.set_program_artifact(input.program_artifact());
    protocol::validate_event_set_review_input(collection);
    std::string collection_result;
    {
        tasks::Store store(root, {"research-one"});
        programs::Files programmes(root / "programs", input.owner());
        (void)programmes.preserve(current_executable(), input.program_artifact());
        auto submission = task_submission(store, "joint-audit", input);
        store.admit_submission(submission);
        submission.prepare_files();
        const auto binding = tasks::notice_consumer_binding(submission.execution(), "joint-audit");
        const auto held = f.notices->prepare_consumer(binding);
        tasks::save_notice_custody(root / "joint-audit", held);
        store.commit(store.register_submission(submission));
        const auto confirmed = f.notices->confirm_consumer(binding);
        tasks::save_notice_custody(root / "joint-audit", confirmed);
        auto attempt = notice_attempt(store, "joint-audit");
        store.commit(
            tasks::finish(
                store,
                tasks::execute(attempt, root, {}, [](auto, auto) {}, input.program_artifact())));
        auto set_submission = task_submission(store, "joint-set-audit", collection);
        store.admit_submission(set_submission);
        set_submission.prepare_files();
        const auto set_binding =
            tasks::notice_consumer_binding(set_submission.execution(), "joint-set-audit");
        tasks::save_notice_custody(
            root / "joint-set-audit",
            f.notices->prepare_consumer(set_binding));
        store.commit(store.register_submission(set_submission));
        tasks::save_notice_custody(
            root / "joint-set-audit",
            f.notices->confirm_consumer(set_binding));
        auto set_read = store.prepare_input("joint-set-audit");
        set_read.load_for_claim();
        const auto token = store.commit(store.claim(set_read)).token();
        auto set_attempt = store.confirm_attempt(std::move(set_read));
        set_attempt.set_token(token);
        store.commit(
            tasks::finish(
                store,
                tasks::execute(
                    set_attempt,
                    root,
                    {},
                    [](auto, auto) {},
                    collection.program_artifact())));
        collection_result =
            tasks::verified_result(store, "joint-set-audit").event_set_review().SerializeAsString();
    }
    f.notices.reset();
    f.retentions.reset();
    f.data.reset();
    const std::vector<terminal_backup::Selection> selected{
        {f.data_owner, f.data_root},
        {f.notice_owner, f.notice_root},
        {input.owner(), root}
    };
    {
        auto cut = terminal_backup::cold_cut(selected);
        const auto plan = cut.preview("notice-joint");
        unsigned business = 0;
        for (const auto& dependency : plan.at("dependencies")) {
            const auto kind = dependency.at("object").at("kind").get<std::string>();
            if (kind == "intelligence_publication" || kind == "intelligence_content"
                || kind == "intelligence_preparation" || kind == "table_snapshot"
                || kind == "material_receipt" || kind == "material_original"
                || kind == "research_program" || kind == "intelligence_program"
                || kind == "intelligence_set" || kind == "intelligence_set_content") {
                EXPECT_TRUE(dependency.at("included").get<bool>()) << kind;
                ++business;
            }
        }
        EXPECT_GE(business, 7U);
        cut.save("notice-joint", f.folder.path / "joint-backup", sha256_bytes(plan.dump()));
    }
    backup_files::restore_files(f.folder.path / "joint-backup", f.folder.path / "joint-restored");
    std::vector<terminal_backup::Selection> restored;
    const auto manifest = backup_files::inspect(f.folder.path / "joint-backup");
    unsigned index = 0;
    for (const auto& identity : manifest.at("owners")) {
        auto fixed = owner(identity.at("domain"), identity.at("service"));
        restored.push_back(
            {fixed, f.folder.path / "joint-restored/owners" / std::to_string(index++)});
    }
    {
        auto cut = terminal_backup::cold_cut(restored);
        const auto plan = cut.preview("restored-notice-joint");
        EXPECT_EQ(plan.at("dependencies").size(), manifest.at("dependencies").size());
    }
    const auto restored_research = std::ranges::find_if(restored, [&](const auto& item) {
        return item.owner.SerializeAsString() == input.owner().SerializeAsString();
    });
    ASSERT_NE(restored_research, restored.end());
    {
        tasks::Store recovered(restored_research->directory, {"research-one"});
        const auto result = tasks::verified_result(recovered, "joint-set-audit");
        EXPECT_EQ(result.event_set_review().SerializeAsString(), collection_result);
        EXPECT_EQ(
            result.result_task().event_set_review().SerializeAsString(),
            collection.SerializeAsString());
    }
    const auto original = sha256_file(f.notice_root / "ledger.sqlite");
    const auto source = std::ranges::find_if(restored, [&](const auto& item) {
        return item.owner.SerializeAsString() == f.notice_owner.SerializeAsString();
    });
    ASSERT_NE(source, restored.end());
    {
        sqlite::Database db(source->directory / "ledger.sqlite");
        db.execute("DELETE FROM notice_consumers");
    }
    EXPECT_THROW(terminal_backup::cold_cut(restored), Error);
    EXPECT_EQ(sha256_file(f.notice_root / "ledger.sqlite"), original);
}

TEST(NoticeLedgerBackup, ReadOnlyCutRetainsHumanAndPublicationAuthorityAcrossIndependentRestore)
{
    Fixture f;
    const auto input = published_notice_input(f);
    const auto source = input.request().source();
    EXPECT_THROW(intelligence::cold_backup(f.notice_root, f.notice_owner), std::exception);
    f.notices.reset();
    const auto before = sha256_file(f.notice_root / "ledger.sqlite");
    {
        auto exported = intelligence::cold_backup(f.notice_root, f.notice_owner);
        EXPECT_TRUE(std::ranges::any_of(exported.objects, [&](const auto& object) {
            return object.SerializeAsString() == source.SerializeAsString();
        }));
        std::vector<backup_files::Snapshot> snapshots;
        snapshots.push_back(std::move(exported.snapshot));
        backup_files::Cut cut(std::move(snapshots));
        const auto plan = cut.preview("notice-ledger");
        cut.save("notice-ledger", f.folder.path / "ledger-backup", sha256_bytes(plan.dump()));
    }
    EXPECT_EQ(sha256_file(f.notice_root / "ledger.sqlite"), before);
    backup_files::restore_files(f.folder.path / "ledger-backup", f.folder.path / "ledger-restored");
    const auto root = f.folder.path / "ledger-restored/owners/0";
    {
        auto verified = intelligence::cold_backup(root, f.notice_owner);
        EXPECT_FALSE(verified.objects.empty());
    }
    {
        intelligence::Store recovered(root, f.notice_owner);
        EXPECT_EQ(
            recovered.publication(source).SerializeAsString(),
            input.publication().SerializeAsString());
    }
    {
        sqlite::Database writer(root / "ledger.sqlite");
        writer.execute("DELETE FROM notice_operations");
    }
    EXPECT_THROW(intelligence::cold_backup(root, f.notice_owner), std::invalid_argument);
    EXPECT_EQ(sha256_file(f.notice_root / "ledger.sqlite"), before);
}

TEST(NoticeContentBackup, IndependentlyRestoresSavedBytesAndRejectsCorruptOrUnknownEvidence)
{
    Fixture f;
    const auto input = published_notice_input(f);
    const auto origin = input.content().receipt().origin();
    data::proto::DataOwner selected;
    selected.set_node_id(f.data_owner.node_id());
    selected.set_service_id(f.data_owner.service_id());
    f.retentions.reset();
    f.data.reset();
    const auto before = sha256_file(f.data_root / "intelligence-content.required.pb");
    {
        auto exported = data::cold_backup(f.data_root, selected);
        EXPECT_TRUE(std::ranges::any_of(exported.objects, [&](const auto& object) {
            return object.SerializeAsString()
                == input.content().receipt().content().SerializeAsString();
        }));
        EXPECT_TRUE(
            std::ranges::any_of(exported.snapshot.dependencies, [&](const auto& dependency) {
                return dependency.object.SerializeAsString() == origin.SerializeAsString();
            }));
        std::vector<backup_files::Snapshot> snapshots;
        snapshots.push_back(std::move(exported.snapshot));
        backup_files::Cut cut(std::move(snapshots));
        const auto plan = cut.preview("content-cold");
        cut.save("content-cold", f.folder.path / "content-backup", sha256_bytes(plan.dump()));
    }
    EXPECT_EQ(sha256_file(f.data_root / "intelligence-content.required.pb"), before);
    backup_files::restore_files(
        f.folder.path / "content-backup",
        f.folder.path / "content-restored");
    const auto root = f.folder.path / "content-restored/owners/0";
    data::IntelligenceContent content(root, f.data_owner);
    EXPECT_EQ(
        content.read(origin).payload.SerializeAsString(),
        input.content().payload().SerializeAsString());
    {
        auto inspected = data::cold_backup(root, selected);
        EXPECT_FALSE(inspected.objects.empty());
    }
    std::ofstream(root / "intelligence-content/objects" / origin.version(), std::ios::trunc)
        << "corrupt";
    EXPECT_THROW(data::cold_backup(root, selected), std::exception);
}

TEST(NoticeReviewBackup, RestoresOwnedEvidenceAndDeclaresBothSourceDomainsWithoutReadingThem)
{
    Fixture f;
    const auto input = published_notice_input(f);
    const auto root = f.folder.path / "research-backup";
    fs::create_directory(root);
    {
        tasks::Store store(root, {"research-one"});
        programs::Files programmes(root / "programs", input.owner());
        const auto entry = programmes.preserve(current_executable(), input.program_artifact());
        EXPECT_EQ(entry.digest, input.program_artifact());
        tasks::submit(store, "audit-backup", input);
        auto attempt = notice_attempt(store, "audit-backup");
        store.commit(
            tasks::finish(
                store,
                tasks::execute(attempt, root, {}, [](auto, auto) {}, input.program_artifact())));
    }
    const auto before = sha256_file(root / "tasks.sqlite");
    {
        auto exported = research::cold_backup(root, input.owner());
        EXPECT_EQ(exported.objects.size(), 3U);
        ASSERT_EQ(exported.snapshot.dependencies.size(), 3U);
        std::set<std::string> kinds;
        for (const auto& dependency : exported.snapshot.dependencies)
            kinds.insert(dependency.object.kind());
        EXPECT_EQ(
            kinds,
            (std::set<std::string>{
                "intelligence_publication",
                "intelligence_content",
                "research_program"
            }));
        std::vector<backup_files::Snapshot> snapshots;
        snapshots.push_back(std::move(exported.snapshot));
        backup_files::Cut cut(std::move(snapshots));
        const auto plan = cut.preview("notice-cold");
        cut.save("notice-cold", f.folder.path / "backup", sha256_bytes(plan.dump()));
    }
    EXPECT_EQ(sha256_file(root / "tasks.sqlite"), before);
    backup_files::restore_files(f.folder.path / "backup", f.folder.path / "restored");
    fs::rename(root, f.folder.path / "preserved-source");
    const auto restored = f.folder.path / "restored/owners/0";
    {
        auto inspected = research::cold_backup(restored, input.owner());
        EXPECT_EQ(inspected.snapshot.dependencies.size(), 3U);
    }
    tasks::Store store(restored, {"research-one"});
    const auto result = tasks::verified_result(store, "audit-backup");
    EXPECT_EQ(
        result.notice_review().source().SerializeAsString(),
        input.request().source().SerializeAsString());
}

TEST(NoticeReviewStore, CancellationAndInterruptedRetryFenceLateResultsWithoutChangingFixedInput)
{
    Fixture f;
    const auto input = published_notice_input(f);
    const auto root = f.folder.path / "research";
    fs::create_directory(root);
    task::proto::TaskAttempt interrupted;
    {
        tasks::Store store(root, {"research-one"});
        tasks::submit(store, "cancelled", input);
        auto attempt = notice_attempt(store, "cancelled");
        auto late = tasks::execute(attempt, root, {}, [](auto, auto) {}, input.program_artifact());
        store.commit(store.cancel("cancelled"));
        store.commit(tasks::finish(store, std::move(late)));
        EXPECT_EQ(store.describe("cancelled").state(), task::proto::CANCELLED);
        EXPECT_TRUE(store.describe("cancelled").result_digest().empty());
        tasks::submit(store, "interrupted", input);
        interrupted = notice_attempt(store, "interrupted");
    }
    {
        tasks::Store recovered(root, {"research-one"});
        EXPECT_EQ(recovered.describe("interrupted").state(), task::proto::INTERRUPTED);
        EXPECT_EQ(
            recovered.get("interrupted").notice_review().SerializeAsString(),
            input.SerializeAsString());
        recovered.commit(recovered.retry("interrupted"));
        auto attempt = notice_attempt(recovered, "interrupted");
        EXPECT_NE(attempt.token(), interrupted.token());
        auto stale =
            tasks::execute(interrupted, root, {}, [](auto, auto) {}, input.program_artifact());
        EXPECT_THROW((void)tasks::finish(recovered, std::move(stale)), std::invalid_argument);
        auto output =
            tasks::execute(attempt, root, {}, [](auto, auto) {}, input.program_artifact());
        recovered.commit(tasks::finish(recovered, std::move(output)));
        EXPECT_EQ(recovered.describe("interrupted").state(), task::proto::SUCCEEDED);
    }
}

TEST(NoticeEvents, PublicationCreatesEventAndFollowUpKeepsInitialStatementFixed)
{
    Fixture f;
    const auto publish = [&](ip::NoticeCandidate candidate, const std::string& suffix) {
        const auto prepared = f.prepare(candidate);
        f.admit(prepared);
        f.notices->review(f.review(prepared.reference(), "review-" + suffix));
        auto request = f.publish_request(prepared.reference());
        request.set_id("publish-" + suffix);
        const auto intent = f.notices->begin_publication(request);
        auto receipt = f.receipt(intent);
        f.clock->advance(2000000);
        return f.notices->confirm_publication(prepared.reference(), receipt);
    };
    const auto first = publish(f.candidate("event-first"), "first");
    const auto event = f.notices->get("event-first").event();
    EXPECT_EQ(
        event.initial_publication().SerializeAsString(),
        first.reference().SerializeAsString());
    EXPECT_EQ(event.available_at_ms(), first.publication().available_at_ms());
    auto next = f.candidate("event-follow-up");
    *next.mutable_event() = event.reference();
    const auto second = publish(next, "second");
    EXPECT_EQ(f.notices->get(next.id()).event().SerializeAsString(), event.SerializeAsString());
    EXPECT_NE(second.reference().SerializeAsString(), first.reference().SerializeAsString());
    EXPECT_EQ(f.notices->event(event.reference()).SerializeAsString(), event.SerializeAsString());
    const auto original = f.notices->publication(first.reference()).SerializeAsString();
    f.notices.reset();
    f.open();
    EXPECT_EQ(f.notices->event(event.reference()).SerializeAsString(), event.SerializeAsString());
    EXPECT_EQ(f.notices->publication(first.reference()).SerializeAsString(), original);
    auto wrong = next;
    wrong.set_id("event-wrong-contract");
    wrong.set_contract("SHFE/rb/2027-01");
    EXPECT_THROW(f.notices->stage(f.prepare(wrong)), Error);
    wrong = next;
    wrong.set_id("event-wrong-version");
    wrong.mutable_event()->set_version(std::string(64, 'a'));
    EXPECT_THROW(f.notices->stage(f.prepare(wrong)), Error);
    wrong = next;
    wrong.set_id("event-wrong-owner");
    wrong.mutable_event()->mutable_owner()->set_service_id("other-intelligence");
    EXPECT_THROW(f.notices->stage(f.prepare(wrong)), Error);
    f.notices.reset();
    auto lock = intelligence::Store::inspect_backup(
        f.notice_root,
        f.notice_owner,
        [](const ip::NoticeRecord&) {});
    EXPECT_TRUE(lock);
}

TEST(NoticeChanges, DataSavePrecedesAppendAndHistoricalViewsKeepOriginalPublication)
{
    Fixture f(true);
    ip::NoticeDeliveryQuery deliveries;
    deliveries.mutable_filter()->add_contracts(f.candidate("query-only").contract());
    deliveries.mutable_filter()->set_rule_id("contract_margin_announcement");
    deliveries.mutable_filter()->set_use_scope("research");
    deliveries.mutable_filter()->set_include_followup_statements(true);
    for (auto kind : {ip::STATEMENT_PUBLISHED, ip::EVENT_CORRECTED, ip::EVENT_WITHDRAWN})
        deliveries.mutable_filter()->add_kinds(kind);
    deliveries.set_limit(16);
    EXPECT_EQ(f.notices->deliveries(deliveries).head(), 0);
    ip::NoticeSubscriptionDefinition subscription_definition;
    subscription_definition.set_id("event-reminders");
    subscription_definition.set_name("Explicit application event reminders");
    *subscription_definition.mutable_filter() = deliveries.filter();
    subscription_definition.mutable_filter()->set_include_followup_statements(false);
    subscription_definition.set_consumer(ip::NoticeSubscriptionDefinition::APPLICATION_REMINDER);
    EXPECT_TRUE(f.notices->preview_subscription(subscription_definition).items().empty());
    EXPECT_THROW(f.notices->subscription(subscription_definition.id()), Error);
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(f.notices->create_subscription(subscription_definition), std::exception);
    EXPECT_THROW(f.notices->subscription(subscription_definition.id()), Error);
    auto subscription = f.notices->create_subscription(subscription_definition);
    EXPECT_TRUE(subscription.subscription().active());
    EXPECT_EQ(
        f.notices->create_subscription(subscription_definition).SerializeAsString(),
        subscription.SerializeAsString());
    auto different_definition = subscription_definition;
    different_definition.set_name("Another query");
    EXPECT_THROW(f.notices->create_subscription(different_definition), Error);
    const auto offer_request =
        [&](const std::string& id, std::uint64_t after, std::uint64_t through, bool historical) {
            ip::NoticeSubscriptionOffer offer;
            *offer.mutable_subscription() = subscription.subscription().reference();
            offer.set_operation_id(id);
            offer.set_after(after);
            offer.set_through(through);
            offer.set_historical(historical);
            return offer;
        };
    ip::NoticeReminderQuery reminder_query;
    *reminder_query.mutable_subscription() = subscription.subscription().reference();
    reminder_query.set_limit(16);

    data::IntelligenceContent content(f.data_root, f.data_owner, f.clock);
    const auto publish = [&](ip::NoticeCandidate candidate, const std::string& suffix) {
        const auto prepared = f.prepare(candidate);
        f.admit(prepared);
        f.notices->review(f.review(prepared.reference(), "review-" + suffix));
        auto request = f.publish_request(prepared.reference());
        request.set_id("publish-" + suffix);
        const auto intent = f.notices->begin_publication(request);
        const auto receipt = content.save(intent.reference(), [&](const auto&) {
            return intent;
        });
        return f.notices->confirm_publication(prepared.reference(), receipt);
    };
    const auto first = publish(f.candidate("change-first"), "first");
    const auto first_delivery = f.notices->deliveries(deliveries);
    ASSERT_EQ(first_delivery.items_size(), 1);
    EXPECT_EQ(first_delivery.head(), 1);
    EXPECT_EQ(
        first_delivery.items(0).source().SerializeAsString(),
        first.reference().SerializeAsString());
    EXPECT_FALSE(first_delivery.more());
    const auto first_offer_request = offer_request("deliver-first", 0, 1, false);
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(f.notices->offer_delivery(first_offer_request), std::exception);
    EXPECT_FALSE(f.notices->subscription(subscription_definition.id())
            .subscription()
            .has_pending_delivery());
    EXPECT_TRUE(f.notices->reminders(reminder_query).items().empty());
    const auto first_offer = f.notices->offer_delivery(first_offer_request);
    protocol::validate_notice_delivery_batch(first_offer);
    EXPECT_EQ(
        f.notices->offer_delivery(first_offer_request).SerializeAsString(),
        first_offer.SerializeAsString());
    EXPECT_EQ(f.notices->reminders(reminder_query).items_size(), 1);
    EXPECT_EQ(
        f.notices->subscription(subscription_definition.id()).subscription().confirmed_through(),
        0);
    EXPECT_THROW(
        f.notices->offer_delivery(offer_request("competing-delivery", 0, 1, false)),
        Error);
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(f.notices->acknowledge_delivery(first_offer.reference()), std::exception);
    EXPECT_FALSE(f.notices->delivery_batch(first_offer.reference()).acknowledged());
    subscription = f.notices->acknowledge_delivery(first_offer.reference());
    EXPECT_EQ(subscription.subscription().confirmed_through(), 1);
    EXPECT_EQ(
        f.notices->acknowledge_delivery(first_offer.reference()).SerializeAsString(),
        subscription.SerializeAsString());
    EXPECT_TRUE(f.notices->offer_delivery(first_offer_request).acknowledged());

    const auto fixed_original = first.SerializeAsString();
    const auto event = f.notices->get("change-first").event();
    const auto set_request = [&](const std::string& id, std::int64_t cutoff) {
        ip::NoticeSetRequest request;
        request.set_id(id);
        request.set_name("Isolated fixed historical event set");
        *request.mutable_query()->add_events() = event.reference();
        request.mutable_query()->set_use_mode(ip::POST_HOC_RESEARCH);
        request.mutable_query()->set_as_of_ms(cutoff);
        request.mutable_query()->set_use_scope("research");
        *request.mutable_data_owner() = f.data_owner;
        return request;
    };
    auto first_set_request =
        set_request("set-before-correction", first.publication().available_at_ms());
    const auto first_preview = f.notices->preview_set(first_set_request);
    ASSERT_EQ(first_preview.payload().members_size(), 1);
    EXPECT_EQ(
        first_preview.payload().members(0).publication().SerializeAsString(),
        first.SerializeAsString());
    EXPECT_EQ(first_preview.payload().request().program_artifact(), f.programme.digest);
    first_set_request = first_preview.payload().request();
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(f.notices->begin_set(first_set_request), std::exception);
    EXPECT_THROW(f.notices->event_set(first_set_request.id()), Error);
    const auto first_set_intent = f.notices->begin_set(first_set_request);
    EXPECT_FALSE(f.notices->event_set(first_set_request.id()).has_published());
    const auto first_set_saved = content.save_set(first_set_intent.reference(), [&](const auto&) {
        return first_set_intent;
    });
    auto wrong_set_receipt = first_set_saved;
    wrong_set_receipt.mutable_origin()->set_id("another-set");
    EXPECT_THROW(f.notices->confirm_set(first_set_intent.reference(), wrong_set_receipt), Error);
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(
        f.notices->confirm_set(first_set_intent.reference(), first_set_saved),
        std::exception);
    f.notices.reset();
    f.open();
    const auto first_set = f.notices->confirm_set(first_set_intent.reference(), first_set_saved);
    ASSERT_TRUE(first_set.has_published());
    const auto original_set_bytes =
        content.read_set(first_set_intent.reference()).payload.SerializeAsString();
    EXPECT_THROW(content.read(first_set_intent.reference()), std::invalid_argument);
    auto changed_set_request = first_set_request;
    changed_set_request.set_name("another fixed request");
    EXPECT_THROW(f.notices->begin_set(changed_set_request), Error);
    auto wrong_set_target = first_set_request;
    wrong_set_target.set_id("wrong-set-target");
    wrong_set_target.mutable_data_owner()->set_service_id("another-data");
    EXPECT_THROW(f.notices->preview_set(wrong_set_target), Error);
    const auto material = [&](const std::string& id, const std::string& body) {
        auto request = f.material.receipt().evidence().request();
        request.set_id(id);
        data::Materials originals(f.data_root, f.data_owner);
        const auto receipt = originals.publish(
            originals.prepare(request, body, "text/plain", f.clock->utc_now() / 1000000));
        data::proto::MaterialRead read;
        *read.mutable_receipt() = receipt;
        read.set_original(body);
        *read.mutable_text() = protocol::read_material_text(receipt, body);
        return read;
    };
    f.clock->advance(10000000);
    auto replacement_candidate = f.candidate("change-replacement");
    auto revised_bytes = f.body;
    revised_bytes.replace(revised_bytes.find("12%"), 3, "13%");
    f.material = material("corrected-material", revised_bytes);
    *replacement_candidate.mutable_material() = f.material.receipt().reference();
    replacement_candidate.mutable_percentage()->set_text("13%");
    *replacement_candidate.mutable_event() = event.reference();
    const auto replacement = publish(replacement_candidate, "replacement");
    ip::NoticeEventQuery query;
    *query.mutable_event() = event.reference();
    EXPECT_EQ(
        f.notices->event_view(query).selected_publication().SerializeAsString(),
        first.reference().SerializeAsString()); // Publication alone does not correct the event.
    ip::NoticeChangeRequest request;
    request.set_id("correction-one");
    *request.mutable_event() = event.reference();
    *request.mutable_expected_head() = first.reference();
    request.set_action(ip::NoticeChangeRequest::CORRECT);
    *request.mutable_replacement() = replacement.reference();
    *request.mutable_material() = f.material.receipt().reference();
    request.mutable_evidence()->set_begin(0);
    request.mutable_evidence()->set_end(revised_bytes.size());
    request.mutable_evidence()->set_text(revised_bytes);
    request.set_operator_name("isolated human");
    request.set_reason("The new original explicitly replaces the margin rate.");
    request.set_human_confirmed(true);
    auto stale_head = request;
    stale_head.set_id("wrong-head");
    stale_head.mutable_expected_head()->set_version(std::string(64, 'f'));
    EXPECT_THROW(
        f.notices->begin_change(
            intelligence::prepare_notice_change(
                f.notice_owner,
                stale_head,
                f.material,
                f.programme)),
        Error);
    EXPECT_THROW(f.notices->change(stale_head.id()), Error);
    auto unpublished = request;
    unpublished.set_id("unpublished-replacement");
    unpublished.mutable_replacement()->set_id("not-a-published-statement");
    EXPECT_THROW(
        f.notices->begin_change(
            intelligence::prepare_notice_change(
                f.notice_owner,
                unpublished,
                f.material,
                f.programme)),
        Error);
    auto scope_request = f.material.receipt().evidence().request();
    scope_request.set_id("other-scope-original");
    scope_request.set_use_scope("another-purpose");
    data::Materials originals(f.data_root, f.data_owner);
    const auto scope_receipt = originals.publish(originals
            .prepare(scope_request, revised_bytes, "text/plain", f.clock->utc_now() / 1000000));
    data::proto::MaterialRead other_scope;
    *other_scope.mutable_receipt() = scope_receipt;
    other_scope.set_original(revised_bytes);
    *other_scope.mutable_text() = protocol::read_material_text(scope_receipt, revised_bytes);
    auto wrong_scope = request;
    wrong_scope.set_id("wrong-scope");
    *wrong_scope.mutable_material() = scope_receipt.reference();
    EXPECT_THROW(
        f.notices->begin_change(
            intelligence::prepare_notice_change(
                f.notice_owner,
                wrong_scope,
                other_scope,
                f.programme)),
        Error);
    const auto prepared =
        intelligence::prepare_notice_change(f.notice_owner, request, f.material, f.programme);
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(f.notices->begin_change(prepared), std::exception);
    EXPECT_THROW(f.notices->change(request.id()), Error);
    const auto intent = f.notices->begin_change(prepared);
    EXPECT_FALSE(f.notices->change(request.id()).has_published());
    EXPECT_EQ(
        f.notices->deliveries(deliveries).head(),
        2); // Before Data confirmation and after failed commit.

    EXPECT_EQ(f.notices->begin_change(prepared).SerializeAsString(), intent.SerializeAsString());
    ASSERT_TRUE(f.notices->event_view(query).has_pending_change());
    EXPECT_EQ(
        f.notices->event_view(query).pending_change().intent().SerializeAsString(),
        intent.SerializeAsString());
    auto historical_pending = query;
    historical_pending.set_as_of_ms(f.clock->utc_now() / 1000000);
    EXPECT_FALSE(f.notices->event_view(historical_pending).has_pending_change());
    auto concurrent = request;
    concurrent.set_id("correction-concurrent");
    EXPECT_THROW(
        f.notices->begin_change(
            intelligence::prepare_notice_change(
                f.notice_owner,
                concurrent,
                f.material,
                f.programme)),
        Error);
    const auto before = f.clock->utc_now() / 1000000;
    const auto saved = content.save_change(intent.reference(), [&](const auto&) {
        return intent;
    });
    EXPECT_EQ(
        content.read_change(intent.reference()).payload.SerializeAsString(),
        intent.payload().SerializeAsString());
    EXPECT_THROW(content.read(intent.reference()), std::invalid_argument);
    EXPECT_EQ(
        f.notices->event_view(query).head().SerializeAsString(),
        first.reference().SerializeAsString());
    f.clock->advance(10000000);
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(f.notices->confirm_change(intent.reference(), saved), std::exception);
    EXPECT_FALSE(f.notices->change(request.id()).has_published());
    EXPECT_EQ(
        f.notices->deliveries(deliveries).head(),
        2); // Before Data confirmation and after failed commit.

    f.notices.reset();
    f.open();
    const auto correction = f.notices->confirm_change(intent.reference(), saved);
    ASSERT_TRUE(correction.has_published());
    EXPECT_EQ(correction.published().sequence(), 1);
    EXPECT_EQ(f.notices->deliveries(deliveries).head(), 3);
    auto observed = f.notices->subscription(subscription_definition.id());
    ip::NoticeSubscriptionControl observed_gap;
    *observed_gap.mutable_subscription() = observed.subscription().reference();
    observed_gap.set_expected_revision(observed.subscription().revision());
    observed_gap.set_active(true);
    observed_gap.set_gap_through(3);
    observed = f.notices->control_subscription(observed_gap);
    EXPECT_EQ(observed.subscription().confirmed_through(), 1);
    EXPECT_EQ(observed.subscription().gap_through(), 3);
    EXPECT_THROW(
        f.notices->offer_delivery(offer_request("unlabelled-correction-gap", 1, 3, false)),
        Error);
    const auto corrected_offer =
        f.notices->offer_delivery(offer_request("deliver-correction", 1, 3, true));
    ASSERT_EQ(
        corrected_offer.payload().page().items_size(),
        1); // R is a statement, C is the event change.
    EXPECT_EQ(corrected_offer.payload().page().items(0).kind(), ip::EVENT_CORRECTED);
    EXPECT_EQ(corrected_offer.payload().page().scanned_through(), 3);
    subscription = f.notices->acknowledge_delivery(corrected_offer.reference());
    EXPECT_EQ(
        f.notices->reminders(reminder_query).items_size(),
        2); // P/P/C => two changes, one event.
    ip::NoticeSubscriptionControl pause;
    *pause.mutable_subscription() = subscription.subscription().reference();
    pause.set_expected_revision(subscription.subscription().revision());
    pause.set_active(false);
    subscription = f.notices->control_subscription(pause);
    EXPECT_FALSE(subscription.subscription().active());
    EXPECT_EQ(
        f.notices->control_subscription(pause).SerializeAsString(),
        subscription.SerializeAsString());

    EXPECT_EQ(
        f.notices->confirm_change(intent.reference(), saved).SerializeAsString(),
        correction.SerializeAsString());
    EXPECT_EQ(
        f.notices->event_view(query).selected_publication().SerializeAsString(),
        replacement.reference().SerializeAsString());
    auto after_request =
        set_request("set-after-correction", correction.published().available_at_ms());
    const auto after_preview = f.notices->preview_set(after_request);
    ASSERT_EQ(after_preview.payload().members_size(), 1);
    EXPECT_EQ(
        after_preview.payload().members(0).publication().reference().SerializeAsString(),
        replacement.reference().SerializeAsString());
    EXPECT_EQ(
        after_preview.payload().members(0).event().head().SerializeAsString(),
        correction.published().reference().SerializeAsString());
    after_request = after_preview.payload().request();
    const auto after_intent = f.notices->begin_set(after_request);
    const auto after_saved = content.save_set(after_intent.reference(), [&](const auto&) {
        return after_intent;
    });
    const auto after_set = f.notices->confirm_set(after_intent.reference(), after_saved);
    const auto after_set_bytes =
        content.read_set(after_intent.reference()).payload.SerializeAsString();
    research::proto::EventSetReviewInput set_input;
    *set_input.mutable_owner() = owner("research", "research-one");
    *set_input.mutable_request()->mutable_source() = after_set.published().reference();
    set_input.mutable_request()->set_use_scope("research");
    *set_input.mutable_publication() = after_set;
    *set_input.mutable_content()->mutable_receipt() = after_saved;
    *set_input.mutable_content()->mutable_payload() = after_intent.payload();
    set_input.set_program_artifact(f.programme.digest);
    const auto member_read = content.read(replacement.publication().storage().origin());
    ip::NoticeStoredContent member_content;
    *member_content.mutable_receipt() = member_read.receipt;
    *member_content.mutable_payload() = member_read.payload;
    research::proto::NoticeReviewRequest member_request;
    *member_request.mutable_source() = replacement.reference();
    member_request.set_as_of_ms(after_request.query().as_of_ms());
    member_request.set_use_scope(after_request.query().use_scope());
    *set_input.add_members() = protocol::make_notice_review_input(
        set_input.owner(),
        member_request,
        replacement,
        member_content,
        f.programme.digest);
    const auto change_read = content.read_change(intent.reference());
    *set_input.add_changes()->mutable_receipt() = change_read.receipt;
    *set_input.mutable_changes(0)->mutable_payload() = change_read.payload;
    EXPECT_NO_THROW(protocol::validate_event_set_review_input(set_input));
    const auto set_research_root = f.folder.path / "set-research";
    fs::create_directory(set_research_root);
    std::string set_result_digest;
    {
        tasks::Store store(set_research_root, {"research-one"});
        tasks::submit(store, "set-audit", set_input);
        tasks::submit(store, "set-cancel", set_input);
        tasks::submit(store, "set-interrupt", set_input);
        EXPECT_TRUE(
            fs::equivalent(
                set_research_root / "set-audit/input.pb",
                set_research_root / "set-cancel/input.pb"));
        const auto claim_set = [&](const std::string& id) {
            auto read = store.prepare_input(id);
            read.load_for_claim();
            const auto token = store.commit(store.claim(read)).token();
            auto attempt = store.confirm_attempt(std::move(read));
            attempt.set_token(token);
            attempt.set_data_endpoint("/unavailable-set-source.sock");
            attempt.set_data_instance("must-not-be-read");
            return attempt;
        };
        const auto attempt = claim_set("set-audit");
        ASSERT_TRUE(attempt.task().has_event_set_review());
        ASSERT_TRUE(attempt.execution().has_event_set_review());
        EXPECT_FALSE(attempt.execution().has_data_owner());
        unsigned completed = 0;
        auto result = tasks::execute(
            attempt,
            set_research_root,
            {},
            [&](unsigned done, unsigned total) {
                EXPECT_EQ(total, 1U);
                completed = done;
            },
            set_input.program_artifact());
        EXPECT_EQ(completed, 1U);
        protocol::validate_event_set_review_result(set_input, result.event_set_review());
        EXPECT_FALSE(result.event_set_review().members(0).strict_history_eligible());
        auto changed_result = result.event_set_review();
        changed_result.set_as_of_ms(changed_result.as_of_ms() + 1);
        EXPECT_THROW(
            protocol::validate_event_set_review_result(set_input, changed_result),
            std::invalid_argument);
        EXPECT_THROW(
            (void)tasks::execute(
                attempt,
                set_research_root,
                {},
                [](auto, auto) {},
                std::string(64, 'f')),
            Error);
        store.commit(tasks::finish(store, std::move(result)));
        const auto verified = tasks::verified_result(store, "set-audit");
        const auto decoded = protocol::decode_task_result(verified, "set-audit");
        EXPECT_EQ(decoded.at("kind"), "event_set_review");
        EXPECT_EQ(decoded.at("result").at("events").size(), 1);
        EXPECT_EQ(decoded.at("result").at("reports").size(), 1);
        set_result_digest = verified.result_task().result_digest();
        const auto cancelled = claim_set("set-cancel");
        store.commit(store.cancel("set-cancel"));
        std::stop_source stopped;
        stopped.request_stop();
        EXPECT_THROW(
            (void)tasks::execute(
                cancelled,
                set_research_root,
                stopped.get_token(),
                [](auto, auto) {},
                set_input.program_artifact()),
            Error);
        store.commit(store.acknowledge_cancel("set-cancel", cancelled.token()));
        EXPECT_EQ(store.describe("set-cancel").state(), task::proto::CANCELLED);
        (void)claim_set("set-interrupt");
    }
    {
        tasks::Store recovered(set_research_root, {"research-one"});
        const auto result = tasks::verified_result(recovered, "set-audit");
        EXPECT_EQ(result.result_task().result_digest(), set_result_digest);
        EXPECT_EQ(
            result.result_task().event_set_review().SerializeAsString(),
            set_input.SerializeAsString());
        EXPECT_EQ(recovered.describe("set-interrupt").state(), task::proto::INTERRUPTED);
        resource::proto::CatalogueQuery catalogue;
        catalogue.set_kind("event_set_review_result");
        catalogue.set_limit(16);
        EXPECT_EQ(recovered.resources("fixture-node", catalogue).items_size(), 1);
    }
    auto wrong_set_input = set_input;
    wrong_set_input.mutable_members(0)->mutable_request()->set_as_of_ms(before);
    EXPECT_THROW(protocol::validate_event_set_review_input(wrong_set_input), std::invalid_argument);
    wrong_set_input = set_input;
    wrong_set_input.clear_changes();
    EXPECT_THROW(protocol::validate_event_set_review_input(wrong_set_input), std::invalid_argument);
    wrong_set_input = set_input;
    wrong_set_input.mutable_request()->set_use_scope("trading");
    EXPECT_THROW(protocol::validate_event_set_review_input(wrong_set_input), std::invalid_argument);
    auto strict_request = after_request;
    strict_request.set_id("strict-source-time-unknown");
    strict_request.mutable_query()->set_use_mode(ip::STRICT_HISTORY);
    const auto strict_preview = f.notices->preview_set(strict_request);
    EXPECT_TRUE(strict_preview.payload().members().empty());
    ASSERT_EQ(strict_preview.payload().exclusions_size(), 1);
    EXPECT_FALSE(strict_preview.payload().exclusions(0).reasons().empty());
    EXPECT_THROW(f.notices->begin_set(strict_request), Error);
    EXPECT_THROW(f.notices->event_set(strict_request.id()), Error);
    query.set_as_of_ms(before);
    EXPECT_EQ(
        f.notices->event_view(query).selected_publication().SerializeAsString(),
        first.reference().SerializeAsString());
    query.clear_as_of_ms();
    f.clock->advance(10000000);
    const auto withdrawn_evidence =
        material("withdrawal-material", "The corrected announcement is withdrawn.");
    auto withdrawal = request;
    withdrawal.set_id("withdrawal-one");
    withdrawal.set_action(ip::NoticeChangeRequest::WITHDRAW);
    withdrawal.clear_replacement();
    *withdrawal.mutable_expected_head() = correction.published().reference();
    *withdrawal.mutable_material() = withdrawn_evidence.receipt().reference();
    withdrawal.mutable_evidence()->set_end(withdrawn_evidence.original().size());
    withdrawal.mutable_evidence()->set_text(withdrawn_evidence.original());
    withdrawal.set_reason("The new original withdraws this announcement.");
    const auto withdraw_intent = f.notices->begin_change(
        intelligence::prepare_notice_change(
            f.notice_owner,
            withdrawal,
            withdrawn_evidence,
            f.programme));
    const auto stored_withdrawal =
        content.save_change(withdraw_intent.reference(), [&](const auto&) {
            return withdraw_intent;
        });
    const auto before_withdrawal = f.clock->utc_now() / 1000000;
    f.clock->advance(10000000);
    const auto withdrawn =
        f.notices->confirm_change(withdraw_intent.reference(), stored_withdrawal);
    EXPECT_EQ(withdrawn.published().sequence(), 2);
    const auto delivery_history = f.notices->deliveries(deliveries);
    ASSERT_EQ(delivery_history.items_size(), 4);
    EXPECT_EQ(delivery_history.head(), 4);
    EXPECT_EQ(delivery_history.scanned_through(), 4);
    EXPECT_THROW(f.notices->offer_delivery(offer_request("paused-attempt", 3, 4, false)), Error);
    EXPECT_EQ(f.notices->reminders(reminder_query).items_size(), 2);
    auto resume = pause;
    resume.set_expected_revision(subscription.subscription().revision());
    resume.set_active(true);
    subscription = f.notices->control_subscription(resume);
    EXPECT_EQ(subscription.subscription().confirmed_through(), 3);
    EXPECT_EQ(subscription.subscription().gap_through(), 4); // Reconnection alone did not fill it.
    EXPECT_THROW(f.notices->offer_delivery(offer_request("unlabelled-gap", 3, 4, false)), Error);
    const auto historical_offer =
        f.notices->offer_delivery(offer_request("backfill-withdrawal", 3, 4, true));
    EXPECT_TRUE(historical_offer.payload().request().historical());
    EXPECT_EQ(historical_offer.payload().page().items_size(), 1);
    EXPECT_EQ(
        f.notices->subscription(subscription_definition.id()).subscription().confirmed_through(),
        3);

    EXPECT_EQ(
        delivery_history.items(0).source().SerializeAsString(),
        first.reference().SerializeAsString());
    EXPECT_EQ(
        delivery_history.items(1).source().SerializeAsString(),
        replacement.reference().SerializeAsString());
    EXPECT_EQ(delivery_history.items(2).kind(), ip::EVENT_CORRECTED);
    EXPECT_EQ(
        delivery_history.items(2).source().SerializeAsString(),
        correction.published().reference().SerializeAsString());
    EXPECT_EQ(delivery_history.items(3).kind(), ip::EVENT_WITHDRAWN);
    EXPECT_EQ(
        delivery_history.items(3).source().SerializeAsString(),
        withdrawn.published().reference().SerializeAsString());
    auto heads_only = deliveries;
    heads_only.mutable_filter()->set_include_followup_statements(false);
    const auto event_changes = f.notices->deliveries(heads_only);
    ASSERT_EQ(event_changes.items_size(), 3);
    EXPECT_EQ(event_changes.items(0).position(), 1);
    EXPECT_EQ(event_changes.items(1).position(), 3);
    EXPECT_EQ(event_changes.items(2).position(), 4);
    EXPECT_EQ(event_changes.scanned_through(), 4);
    auto first_page_query = deliveries;
    first_page_query.set_through(2);
    first_page_query.set_limit(1);
    const auto first_page = f.notices->deliveries(first_page_query);
    ASSERT_EQ(first_page.items_size(), 1);
    EXPECT_EQ(first_page.scanned_through(), 1);
    EXPECT_TRUE(first_page.more());
    auto second_page_query = first_page_query;
    second_page_query.set_after(first_page.scanned_through());
    const auto second_page = f.notices->deliveries(second_page_query);
    ASSERT_EQ(second_page.items_size(), 1);
    EXPECT_EQ(second_page.scanned_through(), 2);
    EXPECT_FALSE(second_page.more());
    auto unmatched_query = deliveries;
    unmatched_query.mutable_filter()->set_use_scope("another-purpose");
    const auto unmatched = f.notices->deliveries(unmatched_query);
    EXPECT_TRUE(unmatched.items().empty());
    EXPECT_EQ(unmatched.scanned_through(), 4);
    EXPECT_FALSE(unmatched.more());
    auto future_query = deliveries;
    future_query.set_through(5);
    EXPECT_THROW(f.notices->deliveries(future_query), Error);
    future_query.clear_through();
    future_query.set_after(4);
    EXPECT_TRUE(f.notices->deliveries(future_query).items().empty());

    EXPECT_TRUE(f.notices->event_view(query).withdrawn());
    const auto withdrawn_preview = f.notices->preview_set(
        set_request("set-after-withdrawal", withdrawn.published().available_at_ms()));
    EXPECT_TRUE(withdrawn_preview.payload().members().empty());
    ASSERT_EQ(withdrawn_preview.payload().exclusions_size(), 1);
    EXPECT_EQ(
        withdrawn_preview.payload().exclusions(0).reasons(0).reason(),
        "event_withdrawn_by_cutoff");
    EXPECT_THROW(f.notices->begin_set(withdrawn_preview.payload().request()), Error);
    EXPECT_EQ(
        f.notices->set_publication(first_set.published().reference()).SerializeAsString(),
        first_set.SerializeAsString());
    EXPECT_EQ(
        content.read_set(first_set_intent.reference()).payload.SerializeAsString(),
        original_set_bytes);
    EXPECT_EQ(
        content.read_set(after_intent.reference()).payload.SerializeAsString(),
        after_set_bytes);
    bool fetched_set = false;
    EXPECT_EQ(
        content
            .save_set(
                first_set_intent.reference(),
                [&](const auto&) -> ip::NoticeSetIntent {
                    fetched_set = true;
                    throw std::runtime_error("offline");
                })
            .SerializeAsString(),
        first_set_saved.SerializeAsString());
    EXPECT_FALSE(fetched_set);
    query.set_as_of_ms(before_withdrawal);
    EXPECT_FALSE(f.notices->event_view(query).withdrawn());
    EXPECT_EQ(
        f.notices->event_view(query).selected_publication().SerializeAsString(),
        replacement.reference().SerializeAsString());
    EXPECT_EQ(f.notices->publication(first.reference()).SerializeAsString(), fixed_original);
    bool fetched = false;
    EXPECT_EQ(
        content
            .save_change(
                intent.reference(),
                [&](const auto&) -> ip::NoticeChangeIntent {
                    fetched = true;
                    throw std::runtime_error("offline");
                })
            .SerializeAsString(),
        saved.SerializeAsString());
    EXPECT_FALSE(fetched);
    f.notices.reset();
    f.open();
    query.clear_as_of_ms();
    EXPECT_TRUE(f.notices->event_view(query).withdrawn());
    EXPECT_EQ(f.notices->publication(first.reference()).SerializeAsString(), fixed_original);
    EXPECT_EQ(
        f.notices->deliveries(deliveries).SerializeAsString(),
        delivery_history.SerializeAsString());
    subscription = f.notices->subscription(subscription_definition.id());
    EXPECT_EQ(
        subscription.subscription().pending_delivery().SerializeAsString(),
        historical_offer.reference().SerializeAsString());
    EXPECT_EQ(
        f.notices->delivery_batch(historical_offer.reference()).SerializeAsString(),
        historical_offer.SerializeAsString());
    auto wrong_ack = historical_offer.reference();
    wrong_ack.set_version(std::string(64, 'f'));
    EXPECT_THROW(f.notices->acknowledge_delivery(wrong_ack), Error);
    subscription = f.notices->acknowledge_delivery(historical_offer.reference());
    EXPECT_EQ(subscription.subscription().confirmed_through(), 4);
    EXPECT_EQ(subscription.subscription().gap_through(), 0);
    const auto reminders = f.notices->reminders(reminder_query);
    ASSERT_EQ(reminders.items_size(), 3);
    EXPECT_FALSE(reminders.items(0).historical());
    EXPECT_TRUE(reminders.items(1).historical());
    EXPECT_TRUE(reminders.items(2).historical());

    auto stale = request;
    stale.set_id("stale-change");
    EXPECT_THROW(
        f.notices->begin_change(
            intelligence::prepare_notice_change(f.notice_owner, stale, f.material, f.programme)),
        Error);
    auto wrong = request;
    wrong.mutable_evidence()->set_text("unquoted interpretation");
    EXPECT_THROW(
        intelligence::prepare_notice_change(f.notice_owner, wrong, f.material, f.programme),
        std::exception);
    wrong = request;
    wrong.set_human_confirmed(false);
    EXPECT_THROW(
        intelligence::prepare_notice_change(f.notice_owner, wrong, f.material, f.programme),
        std::invalid_argument);
    resource::proto::CatalogueQuery catalogue;
    catalogue.set_limit(10);
    catalogue.set_kind("intelligence_event");
    EXPECT_EQ(f.notices->resources(catalogue).items_size(), 1);
    catalogue.set_kind("intelligence_change");
    EXPECT_EQ(f.notices->resources(catalogue).items_size(), 2);
    catalogue.set_kind("intelligence_change_preparation");
    EXPECT_EQ(f.notices->resources(catalogue).items_size(), 2);
    catalogue.set_kind("intelligence_content");
    EXPECT_EQ(f.data->resources(f.data_owner, catalogue, {}).items_size(), 2);
    catalogue.set_kind("intelligence_change_content");
    EXPECT_EQ(f.data->resources(f.data_owner, catalogue, {}).items_size(), 2);
    catalogue.set_kind("intelligence_set");
    EXPECT_EQ(f.notices->resources(catalogue).items_size(), 2);
    catalogue.set_kind("intelligence_set_content");
    EXPECT_EQ(f.data->resources(f.data_owner, catalogue, {}).items_size(), 2);
    f.notices.reset();
    f.retentions.reset();
    f.data.reset();
    const std::vector<terminal_backup::Selection> domains{
        {f.data_owner, f.data_root},
        {f.notice_owner, f.notice_root}
    };
    const auto backup = f.folder.path / "change-backup";
    {
        auto cut = terminal_backup::cold_cut(domains);
        const auto plan = cut.preview("changes");
        for (const auto& dependency : plan.at("dependencies"))
            EXPECT_TRUE(dependency.at("included").get<bool>()) << dependency.dump();
        cut.save("changes", backup, sha256_bytes(plan.dump()));
    }
    const auto restored_root = f.folder.path / "change-restored";
    backup_files::restore_files(backup, restored_root);
    std::vector<terminal_backup::Selection> restored;
    fs::path restored_intelligence, restored_data;
    unsigned index = 0;
    const auto manifest = backup_files::inspect(backup);
    for (const auto& identity : manifest.at("owners")) {
        auto fixed = owner(identity.at("domain"), identity.at("service"));
        const auto directory = restored_root / "owners" / std::to_string(index++);
        restored.push_back({fixed, directory});
        if (fixed.domain() == "intelligence")
            restored_intelligence = directory;
        if (fixed.domain() == "data")
            restored_data = directory;
    }
    {
        auto verified = terminal_backup::cold_cut(restored);
    }
    ASSERT_FALSE(restored_intelligence.empty());
    {
        intelligence::Store restored_store(restored_intelligence, f.notice_owner, f.clock);
        EXPECT_TRUE(restored_store.event_view(query).withdrawn());
        EXPECT_EQ(
            restored_store.publication(first.reference()).SerializeAsString(),
            fixed_original);
        EXPECT_EQ(
            restored_store.set_publication(first_set.published().reference()).SerializeAsString(),
            first_set.SerializeAsString());
        EXPECT_EQ(
            restored_store.set_publication(after_set.published().reference()).SerializeAsString(),
            after_set.SerializeAsString());
        ASSERT_FALSE(restored_data.empty());
        const data::IntelligenceContent restored_content(restored_data, f.data_owner, f.clock);
        EXPECT_EQ(
            restored_content.read_set(first_set_intent.reference()).payload.SerializeAsString(),
            original_set_bytes);
        restored_content.verify_set_members(
            restored_content.read_set(after_intent.reference()).payload);

        EXPECT_EQ(
            restored_store.deliveries(deliveries).SerializeAsString(),
            delivery_history.SerializeAsString());
        EXPECT_EQ(
            restored_store.subscription(subscription_definition.id())
                .subscription()
                .SerializeAsString(),
            subscription.subscription().SerializeAsString());
        EXPECT_EQ(
            restored_store.reminders(reminder_query).SerializeAsString(),
            reminders.SerializeAsString());
    }
    {
        sqlite::Database synthetic_damage(restored_intelligence / "ledger.sqlite");
        synthetic_damage.execute("DELETE FROM notice_deliveries WHERE position=2");
    }
    intelligence::Store damaged_store(restored_intelligence, f.notice_owner, f.clock);
    EXPECT_THROW(damaged_store.deliveries(deliveries), Error);
}
