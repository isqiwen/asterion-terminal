#include "material_store.hpp"
#include <asterion/protocol/data_client.hpp>
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <thread>
using namespace asterion;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

struct Process
{
    fs::path root = fs::temp_directory_path() / ("ast-mat-" + unique_process_id());
    std::string endpoint;
    std::unique_ptr<ChildProcess> child;
    unsigned starts = 0;

    Process()
    {
        fs::create_directory(root);
        fs::create_directory(root / "data");
        fs::create_directory(root / "no-plugins");
        endpoint = (root / "p.sock").string();
    }

    ~Process()
    {
        child.reset();
        std::error_code e;
        fs::remove_all(root, e);
    }

    void start(bool provider = true)
    {
        child = std::make_unique<ChildProcess>(
            ASTERION_DATA_SERVICE_PATH,
            std::vector<std::string>{
                "--directory",
                (root / "data").string(),
                "--session",
                "data-one",
                "--node-id",
                "fixture-node",
                "--endpoint",
                endpoint,
                "--internal-endpoint",
                (root / "w.sock").string(),
                "--health-endpoint",
                (root / "h.sock").string(),
                "--plugin-directory",
                provider ? std::string(MATERIAL_FIXTURE_DIRECTORY) : (root / "no-plugins").string()
            },
            false,
            root / ("service-" + std::to_string(++starts) + ".log"),
            true);
        protocol::DataClient client(endpoint, "data-one");
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            if (child->exited())
                throw std::runtime_error("isolated Data process exited before readiness");
            try {
                data::proto::DataRequest q;
                q.mutable_heartbeat();
                if (client.call(q, 1s).health().initialized())
                    return;
            } catch (const Error& e) {
                if (e.code() != ErrorCode::unavailable)
                    throw;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("isolated Data readiness timed out");
            std::this_thread::sleep_for(10ms);
        }
    }

    void stop()
    {
        child->request_stop();
        if (!child->wait(5s))
            throw std::runtime_error("isolated Data did not stop");
        EXPECT_EQ(child->exit_code(), 0);
        child.reset();
    }
};

data::proto::MaterialRequest request(
    protocol::DataClient& client,
    const std::string& id,
    const std::string& resource = "announcement")
{
    data::proto::DataRequest q;
    q.mutable_material_sources();
    const auto rows = client.call(q, 2s).material_sources();
    const auto source = std::find_if(rows.items().begin(), rows.items().end(), [](const auto& s) {
        return s.id() == "fixture-material";
    });
    if (source == rows.items().end())
        throw std::runtime_error("isolated material source not listed");
    data::proto::MaterialRequest r;
    r.set_id(id);
    r.set_source_id(source->id());
    r.set_resource_id(resource);
    r.set_use_scope("isolated synthetic research");
    r.set_provider_artifact(source->provider_artifact());
    r.set_max_requests(1);
    r.set_max_bytes(source->max_bytes());
    return r;
}

task::proto::Task submit(protocol::DataClient& client, const data::proto::MaterialRequest& r)
{
    data::proto::DataRequest q;
    *q.mutable_acquisition_task()->mutable_material_submit() = r;
    return client.call(q, 3s).acquisition_task().task();
}

task::proto::Task get(protocol::DataClient& client, const std::string& id)
{
    data::proto::DataRequest q;
    q.mutable_acquisition_task()->mutable_get()->set_id(id);
    return client.call(q, 2s).acquisition_task().task();
}

task::proto::Task await(protocol::DataClient& client, const std::string& id, bool running = false)
{
    const auto until = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        const auto t = get(client, id);
        if (running && t.state() == task::proto::RUNNING)
            return t;
        if (t.state() != task::proto::RUNNING && t.state() != task::proto::QUEUED
            && t.state() != task::proto::CANCEL_REQUESTED && t.state() != task::proto::PUBLISHING)
            return t;
        if (std::chrono::steady_clock::now() >= until)
            throw std::runtime_error("material task wait timed out");
        std::this_thread::sleep_for(10ms);
    }
}

} // namespace

TEST(MaterialProcess, AcquiresThroughRealDataServiceAndKeepsOriginalWithoutProviderAfterRestart)
{
    Process p;
    p.start();
    protocol::DataClient client(p.endpoint, "data-one");
    const auto plan = request(client, "one");
    const auto admitted = submit(client, plan);
    EXPECT_EQ(admitted.kind(), task::proto::MATERIAL_ACQUISITION);
    const auto completed = await(client, "one");
    ASSERT_EQ(completed.state(), task::proto::SUCCEEDED) << completed.error();
    data::proto::DataRequest read;
    read.mutable_material_read()->set_id("one");
    const auto first = client.call(read, 2s).material_read();
    EXPECT_EQ(first.original(), "工厂甲计划检修\n");
    auto fixed = read;
    *fixed.mutable_material_read()->mutable_expected_reference() = first.receipt().reference();
    EXPECT_EQ(
        client.call(fixed, 2s).material_read().SerializeAsString(),
        first.SerializeAsString());
    fixed.mutable_material_read()->mutable_expected_reference()->set_version(std::string(64, 'f'));
    EXPECT_THROW(client.call(fixed, 2s), Error);
    EXPECT_EQ(client.call(read, 2s).material_read().SerializeAsString(), first.SerializeAsString());
    EXPECT_EQ(first.text().state(), data::proto::MaterialText::READABLE);
    EXPECT_FALSE(first.receipt().evidence().has_source_published_at_ms());
    EXPECT_EQ(first.receipt().reference().owner().node_id(), "fixture-node");
    EXPECT_TRUE(
        fs::is_regular_file(p.root / "data/programs" / (plan.provider_artifact() + ".so"))
        || fs::is_regular_file(p.root / "data/programs" / (plan.provider_artifact() + ".dylib")));
    data::proto::DataRequest result;
    result.mutable_acquisition_task()->mutable_result()->set_id("one");
    EXPECT_EQ(
        client.call(result, 2s).acquisition_task().material().SerializeAsString(),
        first.receipt().SerializeAsString());
    auto changed = plan;
    changed.set_resource_id("different");
    EXPECT_THROW(submit(client, changed), Error);
    p.stop();
    p.start(false);
    data::proto::DataRequest sources;
    sources.mutable_material_sources();
    EXPECT_EQ(client.call(sources, 2s).material_sources().items_size(), 0);
    EXPECT_EQ(client.call(read, 2s).material_read().SerializeAsString(), first.SerializeAsString());
    EXPECT_EQ(submit(client, plan).submission_sequence(), admitted.submission_sequence());
    EXPECT_EQ(
        client.call(result, 2s).acquisition_task().material().SerializeAsString(),
        first.receipt().SerializeAsString());
    auto missing = plan;
    missing.set_id("two");
    EXPECT_THROW(submit(client, missing), Error);
    EXPECT_EQ(get(client, "one").state(), task::proto::SUCCEEDED);
    p.stop();
}

TEST(MaterialProcess, CancellationPreventsPublicationAndQuiescingBlocksNewAcquisition)
{
    Process p;
    p.start();
    protocol::DataClient client(p.endpoint, "data-one");
    const auto plan = request(client, "cancelled", "slow");
    submit(client, plan);
    ASSERT_EQ(await(client, "cancelled", true).state(), task::proto::RUNNING);
    data::proto::DataRequest cancel;
    cancel.mutable_acquisition_task()->mutable_cancel()->set_id("cancelled");
    client.call(cancel, 2s);
    const auto cancelled = await(client, "cancelled");
    EXPECT_EQ(cancelled.state(), task::proto::CANCELLED);
    EXPECT_FALSE(fs::exists(p.root / "data/materials/receipts/cancelled.pb"));
    data::proto::DataRequest quiesce;
    quiesce.mutable_quiesce();
    protocol::DataClient health((p.root / "h.sock").string(), "data-one");
    health.call(quiesce, 2s);
    auto next = plan;
    next.set_id("blocked");
    EXPECT_THROW(submit(client, next), Error);
    EXPECT_FALSE(fs::exists(p.root / "data/acquisition-tasks/blocked"));
    p.stop();
}
