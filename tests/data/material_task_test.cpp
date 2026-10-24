#include "tasks/task_store_support.hpp"
#include <asterion/protocol/material.hpp>
#include <asterion/kernel/process/child.hpp>
#include <gtest/gtest.h>
using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct Folder
{
    fs::path path = fs::temp_directory_path() / ("ast-material-task-" + unique_process_id());

    Folder()
    {
        fs::create_directory(path);
    }

    ~Folder()
    {
        std::error_code e;
        fs::remove_all(path, e);
    }
};

data::proto::MaterialInput input(const std::string& id)
{
    data::proto::MaterialInput value;
    auto& o = *value.mutable_owner();
    o.set_node_id("fixture-node");
    o.set_service_id("data-one");
    o.set_domain("data");
    auto& r = *value.mutable_request();
    r.set_id(id);
    r.set_source_id("fixture-source");
    r.set_resource_id("announcement");
    r.set_use_scope("isolated synthetic research");
    r.set_provider_artifact(std::string(64, 'a'));
    r.set_max_requests(1);
    r.set_max_bytes(4096);
    return value;
}

std::string claim(tasks::Store& store, const std::string& id)
{
    auto read = store.prepare_input(id);
    read.load_for_claim();
    auto change = store.commit(store.claim(read));
    auto attempt = store.confirm_attempt(std::move(read));
    EXPECT_TRUE(attempt.task().has_material());
    EXPECT_EQ(attempt.task().material().SerializeAsString(), input(id).SerializeAsString());
    EXPECT_EQ(
        attempt.execution().parameters_case(),
        task::proto::TaskExecution::PARAMETERS_NOT_SET);
    return change.token();
}

task::proto::TaskFinish finish(const std::string& id, const std::string& token)
{
    task::proto::TaskFinish result;
    result.set_id(id);
    result.set_token(token);
    const auto fixed = input(id);
    *result.mutable_material() =
        protocol::freeze_material(fixed.owner(), fixed.request(), "原始材料\n", "text/plain", 100);
    return result;
}

} // namespace

TEST(MaterialTask, DurableRequestAndReceiptUseExistingQueueAndSurviveSourceOfflineRestart)
{
    Folder f;
    std::string digest;
    {
        tasks::Store store(f.path, {"data-one"});
        const auto submitted = tasks::submit(store, input("one"));
        EXPECT_EQ(submitted.kind(), task::proto::MATERIAL_ACQUISITION);
        EXPECT_EQ(submitted.total(), 1);
        EXPECT_EQ(submitted.provider_artifact(), input("one").request().provider_artifact());
        EXPECT_EQ(
            tasks::submit(store, input("one")).submission_sequence(),
            submitted.submission_sequence());
        auto changed = input("one");
        changed.mutable_request()->set_resource_id("different");
        EXPECT_THROW(tasks::submit(store, changed), std::invalid_argument);
        const auto token = claim(store, "one");
        store.commit(tasks::finish(store, finish("one", token)));
        EXPECT_EQ(store.describe("one").state(), task::proto::SUCCEEDED);
        digest = store.describe("one").result_digest();
        EXPECT_FALSE(store.describe("one").has_material());
    }
    tasks::Store reopened(f.path, {"data-one"});
    const auto result = tasks::verified_result(reopened, "one");
    EXPECT_EQ(result.result_task().result_digest(), digest);
    EXPECT_EQ(
        result.material().evidence().request().SerializeAsString(),
        input("one").request().SerializeAsString());
    const auto json = protocol::decode_task_result(result, "one");
    EXPECT_EQ(json.at("result").at("source_published_at_ms"), nullptr);
    EXPECT_EQ(json.at("experiment").at("resource_id"), "announcement");
}

TEST(MaterialTask, CancellationAndInterruptedRetryKeepInputAndFenceLateAttempt)
{
    Folder f;
    std::string old;
    {
        tasks::Store store(f.path, {"data-one"});
        tasks::submit(store, input("cancelled"));
        const auto token = claim(store, "cancelled");
        store.commit(store.cancel("cancelled"));
        store.commit(tasks::finish(store, finish("cancelled", token)));
        EXPECT_EQ(store.describe("cancelled").state(), task::proto::CANCELLED);
        EXPECT_TRUE(store.describe("cancelled").result_digest().empty());
        tasks::submit(store, input("interrupted"));
        old = claim(store, "interrupted");
    }
    tasks::Store reopened(f.path, {"data-one"});
    EXPECT_EQ(reopened.describe("interrupted").state(), task::proto::INTERRUPTED);
    EXPECT_EQ(
        reopened.get("interrupted").material().SerializeAsString(),
        input("interrupted").SerializeAsString());
    reopened.commit(reopened.retry("interrupted"));
    const auto current = claim(reopened, "interrupted");
    EXPECT_NE(current, old);
    EXPECT_THROW(tasks::finish(reopened, finish("interrupted", old)), std::invalid_argument);
    reopened.commit(tasks::finish(reopened, finish("interrupted", current)));
    EXPECT_EQ(reopened.describe("interrupted").state(), task::proto::SUCCEEDED);
}

TEST(MaterialTask, ForeignOwnerOrChangedAcquisitionCannotBecomeConfirmedResult)
{
    Folder f;
    tasks::Store store(f.path, {"data-one"});
    tasks::submit(store, input("one"));
    const auto token = claim(store, "one");
    const auto wrong = input("other");
    auto result = finish("one", token);
    *result.mutable_material() =
        protocol::freeze_material(wrong.owner(), wrong.request(), "原始材料\n", "text/plain", 100);
    EXPECT_THROW(tasks::finish(store, result), Error);
    EXPECT_EQ(store.describe("one").state(), task::proto::RUNNING);
    auto foreign = input("foreign");
    foreign.mutable_owner()->set_service_id("another-data");
    EXPECT_THROW(tasks::submit(store, foreign), Error);
    store.commit(tasks::finish(store, finish("one", token)));
    EXPECT_EQ(store.describe("one").state(), task::proto::SUCCEEDED);
}
