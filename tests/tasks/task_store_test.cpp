#include "task_execution_support.hpp"

TEST(TaskMetadata, DescriptionAndDerivationSurviveRestartWithoutChangingCalculationEvidence)
{
    TaskDirectory directory;
    const auto spec = input();
    const auto expected = backtest::run(spec);
    std::string input_hash, result_hash, digest;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "original", spec);
        const auto token = store.commit(store.claim("original")).token();
        store.commit(tasks::finish(store, "original", token, expected));
        EXPECT_FALSE(store.describe("original").has_metadata());
        input_hash = sha256_file(directory.path / "original/input.pb");
        result_hash = sha256_file(directory.path / "original/results/1.pb");
        digest = store.describe("original").result_digest();
        auto submission = task_submission(store, "derived", spec);
        task::proto::TaskDerivation source;
        source.set_source_id("original");
        source.set_source_result_digest(digest);
        source.set_data_choice("fixed");
        submission.set_derivation(source);
        store.admit_submission(submission);
        submission.prepare_files();
        store.commit(store.register_submission(submission));
        task::proto::TaskMetadataEdit edit;
        edit.set_id("original");
        edit.set_name("baseline");
        edit.set_note("first line\nsecond line");
        const auto before = store.describe("original");
        const auto changed = store.commit(store.edit_metadata(edit)).task();
        EXPECT_EQ(changed.metadata().revision(), 1U);
        EXPECT_EQ(changed.state(), before.state());
        EXPECT_EQ(changed.attempt(), before.attempt());
        EXPECT_EQ(changed.updated_at_ms(), before.updated_at_ms());
        EXPECT_EQ(changed.result_digest(), digest);
        EXPECT_THROW(store.edit_metadata(edit), Error);
        const auto derived_token = store.commit(store.claim("derived")).token();
        edit.set_id("derived");
        edit.set_name("candidate");
        edit.set_note("fixed evidence");
        store.commit(store.edit_metadata(edit));
        EXPECT_EQ(store.describe("derived").metadata().derivation().source_id(), "original");
        EXPECT_EQ(store.describe("derived").metadata().derivation().source_instance(), "task");
        edit.set_expected_revision(1);
        edit.set_note("must not be committed");
        auto change = store.edit_metadata(edit);
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(change.persist(), std::runtime_error);
        sqlite::fail_next_commits_for_testing(0);
        EXPECT_EQ(store.describe("derived").metadata().note(), "fixed evidence");
        store.commit(store.progress("derived", derived_token, 1));
        store.commit(tasks::finish(store, "derived", derived_token, expected));
    }
    {
        tasks::Store restored(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        const auto original = restored.describe("original"), derived = restored.describe("derived");
        EXPECT_EQ(original.metadata().name(), "baseline");
        EXPECT_EQ(original.metadata().note(), "first line\nsecond line");
        EXPECT_EQ(derived.metadata().name(), "candidate");
        EXPECT_EQ(derived.metadata().revision(), 1U);
        EXPECT_EQ(derived.metadata().derivation().source_result_digest(), digest);
        EXPECT_EQ(derived.metadata().derivation().data_choice(), "fixed");
        EXPECT_EQ(
            tasks::result(restored, "original").SerializeAsString(),
            expected.SerializeAsString());
        EXPECT_EQ(sha256_file(directory.path / "original/input.pb"), input_hash);
        EXPECT_EQ(sha256_file(directory.path / "original/results/1.pb"), result_hash);
    }
}

TEST(TaskEventSegments, FailedIndexCommitKeepsActiveEvidenceAndConfirmsOnlyTheOriginalSegment)
{
    TaskDirectory directory;
    const auto root = std::filesystem::canonical(directory.path);
    const auto segments = root / "event-segments";
    {
        sqlite::Database database(root / "events.sqlite");
        database.execute("CREATE TABLE task_events(task_id TEXT, sequence INTEGER, body TEXT)");
        tasks::TaskEvents log(database, segments, false);
        {
            sqlite::Database::Transaction transaction(database);
            for (unsigned sequence = 1; sequence <= 1000; ++sequence)
                log.append("fixture-task", sequence, "fixture event");
            transaction.commit();
        }
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(log.rotate(), std::runtime_error);
        sqlite::fail_next_commits_for_testing(0);
        ASSERT_TRUE(std::filesystem::exists(segments / "1.pb"));
        const auto hash = sha256_file(segments / "1.pb");
        unsigned count = 0;
        log.read([&](const auto& id, auto sequence, const auto& body) {
            EXPECT_EQ(id, "fixture-task");
            EXPECT_EQ(sequence, ++count);
            EXPECT_EQ(body, "fixture event");
        });
        EXPECT_EQ(count, 1000);
        log.rotate();
        EXPECT_EQ(sha256_file(segments / "1.pb"), hash);
        sqlite::Database::Transaction transaction(database);
        log.append("fixture-task", 1001, "last event");
        transaction.commit();
    }
    sqlite::Database database(root / "events.sqlite", sqlite::Database::Access::read_only);
    tasks::TaskEvents restored(database, segments, true);
    unsigned count = 0;
    restored.read([&](const auto&, auto sequence, const auto&) {
        EXPECT_EQ(sequence, ++count);
    });
    EXPECT_EQ(count, 1001);
    const auto database_hash = sha256_file(root / "events.sqlite");
    replace_file_durably(segments / "1.pb", "corrupted fixture");
    EXPECT_THROW(restored.read([](const auto&, auto, const auto&) {}), std::invalid_argument);
    EXPECT_EQ(sha256_file(root / "events.sqlite"), database_hash);
}

TEST(TaskStore, SegmentedEventsKeepAttemptFencingAndOwnedResultsAcrossRecovery)
{
    TaskDirectory directory;
    auto spec = input();
    auto* dataset = spec.mutable_paper()->mutable_contracts(0)->mutable_dataset();
    const auto first = dataset->bars(0);
    dataset->clear_bars();
    for (std::int64_t i = 0; i < 1100; ++i) {
        auto* bar = dataset->add_bars();
        *bar = first;
        bar->set_timestamp_ns(first.timestamp_ns() + i * 1000000000);
    }
    dataset->set_revision(protocol::bar_dataset_revision(*dataset));
    spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
    const auto expected = backtest::run(spec);
    std::string token, input_hash, result_hash, segment_hash;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "long-run", spec);
        input_hash = sha256_file(directory.path / "long-run/input.pb");
        token = store.commit(store.claim("long-run")).token();
        for (unsigned completed = 1; completed <= 1001; ++completed)
            store.commit(store.progress("long-run", token, completed));
        store.commit(tasks::finish(store, "long-run", token, expected));
        EXPECT_EQ(store.describe("long-run").state(), task::proto::SUCCEEDED);
        result_hash = sha256_file(directory.path / "long-run/results/1.pb");
        segment_hash = sha256_file(directory.path / "event-segments/1.pb");
    }
    {
        tasks::Store restored(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        EXPECT_EQ(restored.describe("long-run").submission_sequence(), 1);
        EXPECT_EQ(restored.describe("long-run").attempt(), 1);
        EXPECT_EQ(
            tasks::result(restored, "long-run").SerializeAsString(),
            expected.SerializeAsString());
        EXPECT_THROW(
            restored.commit(restored.progress("long-run", token, 1002)),
            std::invalid_argument);
        EXPECT_EQ(sha256_file(directory.path / "long-run/input.pb"), input_hash);
        EXPECT_EQ(sha256_file(directory.path / "long-run/results/1.pb"), result_hash);
        EXPECT_EQ(sha256_file(directory.path / "event-segments/1.pb"), segment_hash);
    }
    {
        sqlite::Database database(directory.path / "tasks.sqlite");
        database.execute(
            "DELETE FROM task_events WHERE task_id='long-run' AND sequence=" "(SELECT MAX(sequence) FROM task_events WHERE task_id='long-run')");
    }
    const auto database_hash = sha256_file(directory.path / "tasks.sqlite");
    EXPECT_THROW(
        (tasks::Store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"})),
        std::invalid_argument);
    EXPECT_EQ(sha256_file(directory.path / "tasks.sqlite"), database_hash);
}

TEST(TaskStore, HistoryPageCursorDoesNotHideQueuedWorkOrShiftAfterNewSubmissions)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    const auto spec = input();
    tasks::submit(store, "first", spec);
    tasks::submit(store, "second", spec);
    const auto token = store.commit(store.claim("second")).token();
    store.commit(store.fail("second", token, "fixture failure"));
    tasks::submit(store, "third", spec);
    store.commit(store.cancel("third")).task();
    const auto recent = store.list(2);
    ASSERT_EQ(recent.tasks_size(), 2);
    EXPECT_EQ(recent.tasks(0).id(), "second");
    EXPECT_EQ(recent.tasks(1).id(), "third");
    EXPECT_EQ(recent.next_before_sequence(), 2);
    EXPECT_EQ(recent.failed_count(), 1);
    ASSERT_EQ(recent.active_tasks_size(), 1);
    EXPECT_EQ(recent.active_tasks(0).id(), "first");
    tasks::submit(store, "new-arrival", spec);
    const auto earlier = store.list(2, recent.next_before_sequence());
    ASSERT_EQ(earlier.tasks_size(), 1);
    EXPECT_EQ(earlier.tasks(0).id(), "first");
    EXPECT_EQ(earlier.next_before_sequence(), 0);
    EXPECT_EQ(earlier.failed_count(), 1);
    EXPECT_EQ(earlier.active_tasks_size(), 2);
    std::set<std::string> allowance;

    EXPECT_THROW(store.list(0), std::invalid_argument);
    EXPECT_THROW(store.list(201), std::invalid_argument);
}

TEST(TaskStore, DuplicateSubmissionAndStaleAttemptsAreFenced)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    auto spec = input();
    const auto first = tasks::submit(store, "job1", spec);
    EXPECT_EQ(tasks::submit(store, "job1", spec).SerializeAsString(), first.SerializeAsString());
    spec.mutable_strategies(0)->mutable_quantity()->set_units(d("2").raw());
    EXPECT_THROW(tasks::submit(store, "job1", spec), std::invalid_argument);
    const auto old = store.commit(store.claim("job1")).token();
    store.commit(store.progress("job1", old, 2));
    EXPECT_THROW(store.commit(store.progress("job1", old, 1)), std::invalid_argument);
    EXPECT_EQ(store.commit(store.cancel("job1")).task().state(), task::proto::CANCEL_REQUESTED);
    store.commit(tasks::finish(store, "job1", old, backtest::run(input())));
    EXPECT_EQ(store.get("job1").state(), task::proto::CANCELLED);
    EXPECT_THROW(tasks::result(store, "job1"), std::invalid_argument);
    store.commit(store.retry("job1")).task();
    const auto next = store.commit(store.claim("job1")).token();
    EXPECT_NE(old, next);
    EXPECT_THROW(
        store.commit(tasks::finish(store, "job1", old, backtest::run(input()))),
        std::invalid_argument);
    store.commit(tasks::finish(store, "job1", next, backtest::run(input())));
    EXPECT_EQ(store.get("job1").attempt(), 2U);
    EXPECT_EQ(store.get("job1").state(), task::proto::SUCCEEDED);
    EXPECT_EQ(
        tasks::result(store, "job1").SerializeAsString(),
        backtest::run(input()).SerializeAsString());
    EXPECT_THROW(store.commit(store.retry("job1")).task(), std::invalid_argument);
}

TEST(TaskStore, PreparationFailureRequiresExplicitRetryAndCannotOverwriteCompletedResults)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
    tasks::submit(store, "queued", input());
    store.commit(store.fail_start("queued", 1, "input preparation failed", "operation_failed"));
    EXPECT_EQ(store.get("queued").state(), task::proto::FAILED);
    EXPECT_EQ(store.get("queued").attempt(), 1);
    store.commit(store.retry("queued"));
    store.commit(store.fail_start("queued", 1, "late exit", "operation_failed"));
    EXPECT_EQ(store.get("queued").state(), task::proto::QUEUED);
    auto token = store.commit(store.claim("queued")).token();
    store.commit(store.fail_start("queued", 1, "old exit", "operation_failed"));
    EXPECT_EQ(store.get("queued").state(), task::proto::RUNNING);
    store.commit(tasks::finish(store, "queued", token, backtest::run(input())));
    store.commit(store.fail_start("queued", 3, "late preparation failure", "operation_failed"));
    EXPECT_EQ(store.get("queued").state(), task::proto::SUCCEEDED);
}

TEST(TaskStore, RestartRetainsQueueAndResultsButInterruptsUnconfirmedWork)
{
    TaskDirectory directory;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "queued", input());
        tasks::submit(store, "working", input());
        tasks::submit(store, "done", input());
        const auto token = store.commit(store.claim("working")).token();
        store.commit(store.progress("working", token, 2));
        store.commit(
            tasks::finish(
                store,
                "done",
                store.commit(store.claim("done")).token(),
                backtest::run(input())));
        EXPECT_THROW(
            tasks::Store other(
                asterion::research::task_domain(),
                directory.path,
                tasks::Identity{"task"}),
            std::runtime_error);
    }
    {
        tasks::Store restored(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        EXPECT_EQ(restored.get("queued").state(), task::proto::QUEUED);
        EXPECT_EQ(restored.get("working").state(), task::proto::INTERRUPTED);
        EXPECT_EQ(restored.get("working").completed(), 2U);
        EXPECT_EQ(restored.get("done").state(), task::proto::SUCCEEDED);
        EXPECT_EQ(
            tasks::result(restored, "done").SerializeAsString(),
            backtest::run(input()).SerializeAsString());
        restored.commit(restored.retry("working")).task();
        EXPECT_EQ(restored.get("working").state(), task::proto::QUEUED);
    }
    tasks::Store again(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    EXPECT_EQ(again.get("working").state(), task::proto::QUEUED);
}

TEST(TaskStore, InstanceBindingRejectsWrongOwnerBeforeRecovery)
{
    TaskDirectory directory;
    const tasks::Identity identity{"task-one"};
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, identity);
        tasks::submit(store, "working", input());
        (void)store.commit(store.claim("working")).token();
    }
    const auto database = directory.path / "tasks.sqlite";
    const auto seal = directory.path / "instance.json";
    const auto database_hash = sha256_file(database);
    const auto seal_hash = sha256_file(seal);
    for (const tasks::Identity wrong : {tasks::Identity{"task-two"}, tasks::Identity{"data-one"}}) {
        EXPECT_THROW(
            tasks::Store rejected(asterion::research::task_domain(), directory.path, wrong),
            std::invalid_argument);
        EXPECT_THROW(
            tasks::Store::inspect_history_usage(
                asterion::research::task_domain(),
                directory.path,
                wrong,
                std::string(64, 'a')),
            std::invalid_argument);
        EXPECT_EQ(sha256_file(database), database_hash);
        EXPECT_EQ(sha256_file(seal), seal_hash);
    }
    tasks::Store restored(asterion::research::task_domain(), directory.path, identity);
    EXPECT_EQ(restored.get("working").state(), task::proto::INTERRUPTED);
    EXPECT_EQ(sha256_file(seal), seal_hash);
}

TEST(TaskStore, MissingOrUnsupportedIdentityIsNeverFilledIntoExistingStore)
{
    for (const bool missing : {true, false}) {
        SCOPED_TRACE(missing);
        TaskDirectory directory;
        const tasks::Identity identity{"task-one"};
        {
            tasks::Store store(asterion::research::task_domain(), directory.path, identity);
            tasks::submit(store, "working", input());
            (void)store.commit(store.claim("working")).token();
        }
        const auto seal = directory.path / "instance.json";
        if (missing)
            std::filesystem::remove(seal);
        else
            replace_file_durably(seal, Json{{"task_instance", "different-task"}}.dump());
        const auto database_hash = sha256_file(directory.path / "tasks.sqlite");
        const auto seal_hash = missing ? std::string{} : sha256_file(seal);
        EXPECT_THROW(
            tasks::Store rejected(asterion::research::task_domain(), directory.path, identity),
            std::invalid_argument);
        EXPECT_THROW(
            tasks::Store::inspect_history_usage(
                asterion::research::task_domain(),
                directory.path,
                identity,
                std::string(64, 'a')),
            std::invalid_argument);
        EXPECT_EQ(sha256_file(directory.path / "tasks.sqlite"), database_hash);
        EXPECT_EQ(std::filesystem::exists(seal), !missing);
        if (!missing)
            EXPECT_EQ(sha256_file(seal), seal_hash);
    }
}

TEST(TaskStore, DirectorySyncFailureCannotCommitTaskSubmissionOrSuccessfulResult)
{
    {
        TaskDirectory directory;
        const tasks::Identity identity{"task-one"};
        fail_next_directory_syncs_for_testing(1);
        EXPECT_THROW(
            tasks::Store store(asterion::research::task_domain(), directory.path, identity),
            std::runtime_error);
        fail_next_directory_syncs_for_testing(0);
        EXPECT_FALSE(std::filesystem::exists(directory.path / "tasks.sqlite"));
        tasks::Store restored(asterion::research::task_domain(), directory.path, identity);
        EXPECT_TRUE(restored.list().tasks().empty());
    }
    {
        TaskDirectory directory;
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        fail_next_directory_syncs_for_testing(1);
        EXPECT_THROW(tasks::submit(store, "unpublished", input()), std::runtime_error);
        fail_next_directory_syncs_for_testing(0);
        EXPECT_TRUE(store.list().tasks().empty());
        EXPECT_EQ(store.list().capacity().retained_tasks(), 0U);
        EXPECT_EQ(store.list().capacity().uncommitted(), 1U);
        EXPECT_EQ(store.list().capacity().active_reserved(), 0U);
        EXPECT_EQ(store.list().capacity().active_limit(), 1000U);
    }
    TaskDirectory directory;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "job", input());
        const auto token = store.commit(store.claim("job")).token();
        const auto result = backtest::run(input());
        fail_next_directory_syncs_for_testing(1);
        EXPECT_THROW(store.commit(tasks::finish(store, "job", token, result)), std::runtime_error);
        fail_next_directory_syncs_for_testing(0);
        EXPECT_EQ(store.get("job").state(), task::proto::RUNNING);
        EXPECT_THROW(tasks::result(store, "job"), std::invalid_argument);
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    EXPECT_EQ(restored.get("job").state(), task::proto::INTERRUPTED);
    EXPECT_TRUE(std::filesystem::is_regular_file(directory.path / "job/results/1.pb"));
    restored.commit(restored.retry("job")).task();
    restored.commit(
        tasks::finish(
            restored,
            "job",
            restored.commit(restored.claim("job")).token(),
            backtest::run(input())));
    EXPECT_EQ(restored.get("job").state(), task::proto::SUCCEEDED);
}

TEST(TaskStore, SubmissionPreparationLeavesConfirmedStateAvailable)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    tasks::submit(store, "existing", input());
    auto pending = task_submission(store, "new", input());
    store.admit_submission(pending);
    EXPECT_TRUE(store.has_submission());
    EXPECT_FALSE(std::filesystem::exists(directory.path / "new"));
    EXPECT_EQ(store.list().capacity().active_reserved(), 1U);
    EXPECT_EQ(store.list().capacity().active_used(), 1U);
    EXPECT_EQ(store.list().capacity().retained_tasks(), 1U);
    EXPECT_EQ(store.list().capacity().uncommitted(), 0U);
    auto prepared = std::async(std::launch::async, [&] {
        pending.prepare_files();
    });
    store.commit(store.cancel("existing"));
    prepared.get();
    EXPECT_TRUE(store.active_tasks().empty());
    auto registered = store.register_submission(pending);
    auto persisted = std::async(std::launch::async, [&] {
        registered.persist();
    });
    persisted.get();
    EXPECT_TRUE(store.active_tasks().empty());
    EXPECT_TRUE(store.has_submission());
    store.confirm(registered);
    EXPECT_FALSE(store.has_submission());
    EXPECT_EQ(store.describe("new").submission_sequence(), 2U);
    EXPECT_EQ(store.list().capacity().active_reserved(), 0U);
    EXPECT_EQ(store.list().capacity().active_used(), 1U);

    // Duplicate input verification cannot restore an obsolete state snapshot.
    auto duplicate = task_submission(store, "new", input());
    store.admit_submission(duplicate);
    auto verified = std::async(std::launch::async, [&] {
        duplicate.prepare_files();
    });
    store.commit(store.cancel("new"));
    verified.get();
    EXPECT_EQ(
        store.commit(store.register_submission(duplicate)).task().state(),
        task::proto::CANCELLED);
    EXPECT_TRUE(store.active_tasks().empty());
}

TEST(TaskStore, JournalWaitKeepsActiveStateAvailableAndRequiresOwnerConfirmation)
{
    using namespace std::chrono_literals;
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    tasks::submit(store, "job", input());
    tasks::submit(store, "other", input());
    auto claimed = store.claim("job");
    sqlite::hold_commits_for_testing(true);
    auto persisted = std::async(std::launch::async, [&] {
        claimed.persist();
    });

    struct Release
    {
        ~Release()
        {
            sqlite::hold_commits_for_testing(false);
        }
    } release;

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!sqlite::commit_waiting_for_testing() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_TRUE(sqlite::commit_waiting_for_testing());
    EXPECT_EQ(store.describe("job").state(), task::proto::QUEUED);
    EXPECT_EQ(store.active_tasks().size(), 2U);
    // Another task can be decided while the writer is held at its durable barrier.
    auto cancelled = store.cancel("other");
    EXPECT_EQ(cancelled.task().state(), task::proto::CANCELLED);
    EXPECT_EQ(store.describe("other").state(), task::proto::QUEUED);
    sqlite::hold_commits_for_testing(false);
    persisted.get();
    EXPECT_EQ(store.describe("job").state(), task::proto::QUEUED);
    store.confirm(claimed);
    EXPECT_EQ(store.describe("job").state(), task::proto::RUNNING);
    store.commit(std::move(cancelled));
    ASSERT_EQ(store.active_tasks().size(), 1U);
    EXPECT_EQ(store.active_tasks().front().id(), "job");
}

TEST(TaskStore, FailedStateCommitKeepsTheLastConfirmedActiveState)
{
    TaskDirectory directory;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "job", input());
        const auto token = store.commit(store.claim("job")).token();
        store.commit(store.progress("job", token, 1));
        const auto confirmed = store.describe("job").SerializeAsString();
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(store.commit(store.cancel("job")).task(), std::runtime_error);
        sqlite::fail_next_commits_for_testing(0);
        EXPECT_EQ(store.describe("job").SerializeAsString(), confirmed);
        const auto active = store.active_tasks();
        ASSERT_EQ(active.size(), 1);
        EXPECT_EQ(active.front().SerializeAsString(), confirmed);
        EXPECT_EQ(store.list().capacity().active_used(), 1U);
        EXPECT_THROW(store.commit(store.progress("job", token, 2)), std::runtime_error);
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    EXPECT_EQ(restored.describe("job").state(), task::proto::INTERRUPTED);
    EXPECT_EQ(restored.describe("job").completed(), 1U);
    EXPECT_TRUE(restored.active_tasks().empty());
    restored.commit(restored.retry("job")).task();
    EXPECT_EQ(restored.active_tasks().size(), 1U);
}

TEST(TaskStore, PreparedPayloadCannotBypassCancellationOrANewerAttempt)
{
    for (const bool retry : {false, true}) {
        SCOPED_TRACE(retry);
        TaskDirectory directory;
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "job", input());
        task::proto::TaskFinish request;
        request.set_id("job");
        request.set_token(store.commit(store.claim("job")).token());
        *request.mutable_result() = backtest::run(input());
        auto pending = store.prepare_finish(request);
        EXPECT_THROW(store.commit(store.finish(pending)), std::invalid_argument);
        // File preparation owns only copies. The state owner can process progress,
        // cancellation and a new attempt while the old attempt writes its payload.
        auto preparation = std::async(std::launch::async, [&] {
            pending.prepare_payload();
        });
        store.commit(store.progress("job", request.token(), 1));
        store.commit(store.cancel("job")).task();
        if (retry) {
            store.commit(store.acknowledge_cancel("job", request.token()));
            store.commit(store.retry("job")).task();
            EXPECT_NE(store.commit(store.claim("job")).token(), request.token());
        }
        preparation.get();
        EXPECT_TRUE(store.describe("job").result_digest().empty());
        if (retry) {
            EXPECT_THROW(store.commit(store.finish(std::move(pending))), std::invalid_argument);
            EXPECT_EQ(store.get("job").state(), task::proto::RUNNING);
            EXPECT_EQ(store.get("job").attempt(), 2U);
        } else {
            store.commit(store.finish(std::move(pending)));
            EXPECT_EQ(store.get("job").state(), task::proto::CANCELLED);
        }
        EXPECT_TRUE(std::filesystem::exists(directory.path / "job" / "results" / "1.pb"));
        EXPECT_THROW(tasks::result(store, "job"), std::invalid_argument);
    }
}

TEST(TaskStore, PreparedPayloadIsOwnedAndExpiredAttemptCannotCommit)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    tasks::submit(store, "job", input());
    task::proto::TaskFinish request;
    request.set_id("job");
    request.set_token(store.commit(store.claim("job")).token());
    *request.mutable_result() = backtest::run(input());
    auto pending = store.prepare_finish(request);
    request.mutable_result()->set_dataset_revision("changed.after.prepare");
    EXPECT_NO_THROW(pending.prepare_payload());
    store.commit(store.interrupt("job", request.token(), "test lease expired"));
    EXPECT_THROW(store.commit(store.finish(std::move(pending))), std::invalid_argument);
    EXPECT_EQ(store.get("job").state(), task::proto::INTERRUPTED);
    EXPECT_TRUE(std::filesystem::exists(directory.path / "job" / "results" / "1.pb"));
    EXPECT_THROW(tasks::result(store, "job"), std::invalid_argument);
}

TEST(TaskStore, InputReadPreservesConcurrentCancellationAndRejectsForeignOrChangedFiles)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    tasks::submit(store, "job", input());
    auto read = store.prepare_input("job");
    EXPECT_THROW(store.confirm_input(read), std::invalid_argument);
    EXPECT_THROW((void)store.claim(read), std::invalid_argument);
    auto loading = std::async(std::launch::async, [&] {
        read.load();
    });
    store.commit(store.cancel("job")).task();
    loading.get();
    TaskDirectory foreign_directory;
    tasks::Store foreign(
        asterion::research::task_domain(),
        foreign_directory.path,
        tasks::Identity{"task"});
    tasks::submit(foreign, "job", input());
    EXPECT_THROW(foreign.confirm_input(read), std::invalid_argument);
    EXPECT_THROW((void)foreign.claim(read), std::invalid_argument);
    EXPECT_THROW((void)store.claim(read), std::invalid_argument);
    const auto value = store.confirm_input(std::move(read));
    EXPECT_EQ(value.state(), task::proto::CANCELLED);
    EXPECT_EQ(value.input().SerializeAsString(), input().SerializeAsString());
    EXPECT_THROW(store.commit(store.claim("job")).token(), std::invalid_argument);
    auto changed = store.prepare_input("job");
    std::ofstream(directory.path / "job/input.pb", std::ios::binary | std::ios::trunc) << "bad";
    EXPECT_THROW(changed.load(), std::invalid_argument);
    EXPECT_THROW(store.confirm_input(std::move(changed)), std::invalid_argument);
}

TEST(TaskStore, ResultReadOwnsItsSnapshotAndChecksFilesBeforeCacheConfirmation)
{
    TaskDirectory directory;
    const auto expected = backtest::run(input());
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "done", input());
        store.commit(
            tasks::finish(store, "done", store.commit(store.claim("done")).token(), expected));
    }
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    tasks::submit(store, "other", input());
    const auto token = store.commit(store.claim("other")).token();
    auto read = store.prepare_result("done");
    EXPECT_THROW(store.confirm_result(read), std::invalid_argument);
    auto verifying = std::async(std::launch::async, [&] {
        read.verify();
    });
    store.commit(store.progress("other", token, 1));
    store.commit(store.cancel("other")).task();
    verifying.get();
    TaskDirectory other_directory;
    tasks::Store other(
        asterion::research::task_domain(),
        other_directory.path,
        tasks::Identity{"task"});
    tasks::submit(other, "done", input());
    other.commit(tasks::finish(other, "done", other.commit(other.claim("done")).token(), expected));
    EXPECT_THROW(other.confirm_result(read), std::invalid_argument);
    const auto response = store.confirm_result(std::move(read));
    EXPECT_EQ(response.backtest().SerializeAsString(), expected.SerializeAsString());
    EXPECT_EQ(response.result_task().input().SerializeAsString(), input().SerializeAsString());
    auto cached = store.prepare_result("done");
    std::ofstream(directory.path / "done/input.pb", std::ios::binary | std::ios::trunc) << "bad";
    EXPECT_THROW(cached.verify(), std::invalid_argument);
    EXPECT_THROW(store.confirm_result(std::move(cached)), std::invalid_argument);
}

TEST(TaskStore, VerificationAdmissionReservesBothCompletionsAndReleasesAfterFailure)
{
    tasks::VerificationSlots slots;
    {
        auto read = slots.acquire(false, "read");
        auto first = slots.acquire(true, "first");
        auto second = slots.acquire(true, "second");
        EXPECT_FALSE(slots.completion_available());
        EXPECT_THROW(slots.acquire(false, "another-read"), Error);
        EXPECT_THROW(slots.acquire(true, "third"), Error);
        auto moved = std::move(second);
        EXPECT_THROW(slots.acquire(true, "second"), Error);
    }
    EXPECT_THROW(
        {
            auto read = slots.acquire(false, "failed-read");
            throw std::runtime_error("verification failed");
        },
        std::runtime_error);
    EXPECT_TRUE(slots.completion_available());
    EXPECT_NO_THROW(slots.acquire(false, "next-read"));
    auto first = slots.acquire(true, "first");
    EXPECT_THROW(slots.acquire(true, "first"), Error);
    EXPECT_NO_THROW(slots.acquire(true, "second"));
}

TEST(TaskStore, InvalidInputAndQueuedCancellationDoNotRunAnything)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    auto spec = input();
    spec.set_dataset_revision("invalid");
    EXPECT_THROW(tasks::submit(store, "invalid", spec), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(directory.path / "invalid"));
    EXPECT_THROW(tasks::submit(store, "../outside", input()), std::exception);
    tasks::submit(store, "queued", input());
    EXPECT_EQ(store.commit(store.cancel("queued")).task().state(), task::proto::CANCELLED);
    EXPECT_THROW(store.commit(store.claim("queued")).token(), std::invalid_argument);
    EXPECT_EQ(store.get("queued").attempt(), 0U);
}

TEST(TaskStore, RejectsMismatchedResultContractAndMetrics)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    tasks::submit(store, "job", input());
    const auto token = store.commit(store.claim("job")).token();
    auto result = backtest::run(input());
    result.mutable_account()->mutable_contracts(0)->mutable_contract()->set_symbol("rb2611");
    EXPECT_THROW(store.commit(tasks::finish(store, "job", token, result)), std::invalid_argument);
    result = backtest::run(input());
    result.mutable_max_drawdown()->set_units(0);
    EXPECT_THROW(store.commit(tasks::finish(store, "job", token, result)), std::invalid_argument);
    EXPECT_EQ(store.get("job").state(), task::proto::RUNNING);
    EXPECT_FALSE(std::filesystem::exists(directory.path / "job" / "results" / "1.pb"));
    store.commit(tasks::finish(store, "job", token, backtest::run(input())));
}

TEST(TaskStore, RejectsResultWithDifferentRiskConfiguration)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    const auto spec = input();
    tasks::submit(store, "risk-result", spec);
    const auto token = store.commit(store.claim("risk-result")).token();
    auto result = backtest::run(spec);
    result.mutable_account()->mutable_risk()->set_max_working_orders(999);
    EXPECT_THROW(
        store.commit(tasks::finish(store, "risk-result", token, result)),
        std::invalid_argument);
    EXPECT_EQ(store.get("risk-result").state(), task::proto::RUNNING);
    store.commit(tasks::finish(store, "risk-result", token, backtest::run(spec)));
    EXPECT_EQ(
        tasks::result(store, "risk-result").account().risk().SerializeAsString(),
        spec.paper().risk().SerializeAsString());
}

TEST(TaskStore, SubmissionOrderSurvivesEqualTimesClockRollbackAndRestart)
{
    TaskDirectory directory;
    auto clock = std::make_shared<TaskClock>();
    task::proto::Task first, latest;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"},
            clock);
        first = tasks::submit(store, "z-first", input());
        tasks::submit(store, "a-second", input()); // Same millisecond, opposite ID order.
        clock->value -= 60000000000LL;
        latest = tasks::submit(store, "m-latest", input());
        EXPECT_LT(latest.submitted_at_ms(), first.submitted_at_ms());
        ASSERT_EQ(store.list().tasks_size(), 3);
        EXPECT_EQ(store.list().tasks(0).id(), "z-first");
        EXPECT_EQ(store.list().tasks(1).id(), "a-second");
        EXPECT_EQ(store.list().tasks(2).id(), "m-latest");
        EXPECT_EQ(
            tasks::submit(store, "z-first", input()).SerializeAsString(),
            first.SerializeAsString());
        EXPECT_EQ(latest.submission_sequence(), 3U);
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"},
        clock);
    EXPECT_EQ(restored.get("z-first").SerializeAsString(), first.SerializeAsString());
    EXPECT_EQ(restored.get("m-latest").SerializeAsString(), latest.SerializeAsString());
    EXPECT_EQ(restored.list().tasks(2).id(), "m-latest");
    EXPECT_EQ(tasks::submit(restored, "b-next", input()).submission_sequence(), 4U);
    EXPECT_EQ(restored.list().tasks(3).id(), "b-next");
}

TEST(TaskStore, MetadataUpdatesPreserveLargeImmutableInputsAcrossRecovery)
{
    TaskDirectory directory;
    auto spec = input();
    auto* dataset = spec.mutable_paper()->mutable_contracts(0)->mutable_dataset();
    const auto first = dataset->bars(0);
    dataset->clear_bars();
    for (std::int64_t i = 0; i < 10000; ++i) {
        auto* bar = dataset->add_bars();
        *bar = first;
        bar->set_timestamp_ns(first.timestamp_ns() + i * 1000000000);
    }
    dataset->set_revision(protocol::bar_dataset_revision(*dataset));
    spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
    const auto original = spec.SerializeAsString();
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "large", spec);
        EXPECT_EQ(store.list().capacity().retained_tasks(), 1U);
        EXPECT_EQ(store.list().capacity().uncommitted(), 0U);
        auto expected = store.get("large");
        expected.clear_definition();
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 128; ++i) {
            const auto list = store.list();
            ASSERT_EQ(list.tasks_size(), 1);
            EXPECT_EQ(list.tasks(0).SerializeAsString(), expected.SerializeAsString());
            std::set<std::string> allowance;
        }
        RecordProperty(
            "metadata_128_polls_us",
            std::to_string(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - begin)
                    .count()));
        RecordProperty("immutable_input_bytes", std::to_string(original.size()));
        auto token = store.commit(store.claim("large")).token();
        for (unsigned completed = 1; completed <= 50; ++completed)
            store.commit(store.progress("large", token, completed));
        EXPECT_EQ(store.list().tasks(0).completed(), 50U);
        std::set<std::string> allowance;

        EXPECT_EQ(
            store.commit(store.cancel("large")).task().state(),
            task::proto::CANCEL_REQUESTED);
        store.commit(store.acknowledge_cancel("large", token));
        store.commit(store.retry("large")).task();
        token = store.commit(store.claim("large")).token();
        store.commit(store.progress("large", token, 40));
        store.commit(store.fail("large", token, "explicit test failure"));
        EXPECT_EQ(store.list().tasks(0).error(), "explicit test failure");
        store.commit(store.retry("large")).task();
        token = store.commit(store.claim("large")).token();
        store.commit(store.progress("large", token, 55));
        EXPECT_EQ(store.get("large").input().SerializeAsString(), original);
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    const auto recovered = restored.get("large");
    EXPECT_EQ(recovered.state(), task::proto::INTERRUPTED);
    EXPECT_EQ(recovered.attempt(), 3U);
    EXPECT_EQ(recovered.completed(), 55U);
    EXPECT_EQ(recovered.input().SerializeAsString(), original);
    EXPECT_EQ(
        restored.commit(restored.retry("large")).task().definition_case(),
        task::proto::Task::DEFINITION_NOT_SET);
    EXPECT_EQ(restored.get("large").input().SerializeAsString(), original);
    const auto queued = restored.list().tasks(0);
    EXPECT_EQ(restored.list().capacity().retained_tasks(), 1U);
    EXPECT_EQ(queued.state(), task::proto::QUEUED);
    EXPECT_EQ(queued.total(), 10000U);
    EXPECT_EQ(queued.completed(), 0U);
    EXPECT_TRUE(queued.error().empty());
    EXPECT_EQ(queued.definition_case(), task::proto::Task::DEFINITION_NOT_SET);
}

TEST(TaskStore, BinaryPayloadsRemainCompactAndDefinitionsLoadOnDemand)
{
    TaskDirectory directory;
    const auto spec = input();
    const auto original = spec.SerializeAsString();
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "binary", spec);
    }
    const auto path = directory.path / "binary/input.pb";
    EXPECT_EQ(std::filesystem::file_size(path), original.size());
    EXPECT_EQ(sha256_file(path), sha256_bytes(original));
    {
        sqlite::Database db(directory.path / "tasks.sqlite", sqlite::Database::Access::read_only);
        sqlite::Database::Statement read(db, "SELECT manifest FROM tasks WHERE id='binary'");
        ASSERT_TRUE(read.step());
        const auto manifest = Json::parse(read.text(0));
        EXPECT_EQ(manifest.at("input_bytes"), original.size());
        EXPECT_EQ(manifest.at("input_sha256"), sha256_bytes(original));
        EXPECT_FALSE(manifest.contains("input"));
        EXPECT_LT(read.text(0).size(), 1024U);
    }
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    // Metadata operations never read or retain the input. Explicit input reads
    // and worker claims must still reject missing or changed owned bytes.
    std::filesystem::remove(path);
    EXPECT_EQ(store.list().tasks(0).total(), 7U);
    EXPECT_EQ(store.commit(store.cancel("binary")).task().state(), task::proto::CANCELLED);
    EXPECT_EQ(
        store.commit(store.retry("binary")).task().definition_case(),
        task::proto::Task::DEFINITION_NOT_SET);
    EXPECT_THROW(store.get("binary"), std::invalid_argument);
    EXPECT_THROW(store.commit(store.claim("binary")).token(), std::invalid_argument);
    EXPECT_EQ(store.describe("binary").state(), task::proto::QUEUED);
    write_file_durably(path, original);
    EXPECT_EQ(store.get("binary").input().SerializeAsString(), original);
    const auto expected = backtest::run(spec);
    store.commit(
        tasks::finish(store, "binary", store.commit(store.claim("binary")).token(), expected));
    EXPECT_EQ(
        std::filesystem::file_size(directory.path / "binary/results/1.pb"),
        expected.ByteSizeLong());
    EXPECT_EQ(tasks::result(store, "binary").SerializeAsString(), expected.SerializeAsString());
}

TEST(TaskStore, InvalidPayloadOrPreviousManifestIsRejectedWithoutRewritingFiles)
{
    // Opening does not rescan payloads; reading the fixed input rejects a damaged one.
    for (const auto& mode : {"missing", "truncated", "symlink", "protobuf", "old"}) {
        SCOPED_TRACE(mode);
        TaskDirectory directory;
        {
            tasks::Store store(
                asterion::research::task_domain(),
                directory.path,
                tasks::Identity{"task"});
            tasks::submit(
                store,
                "first",
                input()); // Same digest exercises restored metadata reuse.
            tasks::submit(store, "preserve", input());
            (void)store.commit(store.claim("preserve")).token();
        }
        const auto path = directory.path / "preserve/input.pb";
        const std::string kind = mode;
        if (kind == "missing")
            std::filesystem::remove(path);
        else if (kind == "truncated")
            std::filesystem::resize_file(path, 1);
        else if (kind == "symlink") {
            std::filesystem::rename(path, directory.path / "preserve/other.pb");
            std::filesystem::create_symlink("other.pb", path);
        } else {
            sqlite::Database db(directory.path / "tasks.sqlite");
            Json manifest;
            {
                sqlite::Database::Statement read(
                    db,
                    "SELECT manifest FROM tasks WHERE id='preserve'");
                ASSERT_TRUE(read.step());
                manifest = Json::parse(read.text(0));
            }

            // A matching digest cannot make malformed Protobuf a valid definition.
            write_file_durably(path, "\xff");
            manifest["input_bytes"] = 1;
            manifest["input_sha256"] = sha256_bytes("\xff");
            sqlite::Database::Statement update(
                db,
                "UPDATE tasks SET manifest=? WHERE id='preserve'");
            update.bind(1, manifest.dump()).step();
        }
        const auto input_hash =
            std::filesystem::is_regular_file(path) && !std::filesystem::is_symlink(path)
            ? sha256_file(path)
            : std::string{};
        EXPECT_THROW(
            {
                tasks::Store store(
                    asterion::research::task_domain(),
                    directory.path,
                    tasks::Identity{"task"});
                (void)store.get("preserve");
            },
            std::exception);
        EXPECT_THROW(
            tasks::Store::inspect_history_usage(
                asterion::research::task_domain(),
                directory.path,
                tasks::Identity{"task"},
                std::string(64, 'a')),
            std::exception);
        if (!input_hash.empty())
            EXPECT_EQ(sha256_file(path), input_hash);
        if (kind == "missing")
            EXPECT_FALSE(std::filesystem::exists(path));
        if (kind == "symlink")
            EXPECT_TRUE(std::filesystem::is_symlink(path));
    }
}

TEST(TaskStore, BacktestBeyondTwentyThousandBarsHasReadableResult)
{
    TaskDirectory directory;
    auto spec = input();
    auto* dataset = spec.mutable_paper()->mutable_contracts(0)->mutable_dataset();
    const auto first = dataset->bars(0);
    dataset->clear_bars();
    for (int i = 0; i < 20001; ++i) {
        auto* bar = dataset->add_bars();
        *bar = first;
        bar->set_timestamp_ns(first.timestamp_ns() + static_cast<std::int64_t>(i) * 1000000);
    }
    dataset->set_revision(protocol::bar_dataset_revision(*dataset));
    spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
    const auto expected = backtest::run(spec);
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    tasks::submit(store, "beyond-old-limit", spec);
    store.commit(
        tasks::finish(
            store,
            "beyond-old-limit",
            store.commit(store.claim("beyond-old-limit")).token(),
            expected));
    EXPECT_EQ(
        tasks::result(store, "beyond-old-limit").SerializeAsString(),
        expected.SerializeAsString());
    auto maximum = expected.account();
    maximum.set_total(static_cast<unsigned>(protocol::max_dataset_bars));
    maximum.set_cursor(maximum.total());
    EXPECT_NO_THROW(protocol::decode_snapshot(maximum));
    maximum.set_total(maximum.total() + 1);
    EXPECT_THROW(protocol::decode_snapshot(maximum), std::invalid_argument);
}

TEST(TaskStore, ManyCompletedMaximumInputsRestoreWithOnePayloadWorkingSet)
{
    TaskDirectory directory;
    const auto spec = maximum_factor_input();
    const auto expected = factor::run(spec);
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "batch-0", spec);
        store.commit(
            tasks::finish(
                store,
                "batch-0",
                store.commit(store.claim("batch-0")).token(),
                expected));
    }
    // Build a current-format fixture of independent completed task identities.
    // Hard links only make the test setup cheaper; every task owns its path and
    // the reader must still check every referenced file, digest and transition.
    constexpr int count = 24;
    const auto input_bytes = std::filesystem::file_size(directory.path / "batch-0/input.pb");
    ASSERT_GT(input_bytes * count, 256ULL * 1024 * 1024);
    {
        sqlite::Database db(directory.path / "tasks.sqlite");
        Json base;
        task::proto::StoredTaskRecord base_record;
        {
            sqlite::Database::Statement read(
                db,
                "SELECT manifest, record FROM tasks WHERE id='batch-0'");
            ASSERT_TRUE(read.step());
            base = Json::parse(read.text(0));
            ASSERT_TRUE(base_record.ParseFromString(read.blob(1)));
        }
        sqlite::Database::Transaction transaction(db);
        for (int i = 1; i < count; ++i) {
            const auto id = "batch-" + std::to_string(i);
            const auto dest = directory.path / id;
            std::filesystem::create_directory(dest);
            for (const auto& item :
                std::filesystem::recursive_directory_iterator(directory.path / "batch-0")) {
                const auto target =
                    dest / item.path().lexically_relative(directory.path / "batch-0");
                if (item.is_directory())
                    std::filesystem::create_directory(target);
                else
                    std::filesystem::create_hard_link(item.path(), target);
            }
            auto manifest = base;
            manifest["id"] = id;
            manifest["submission_sequence"] = i + 1;
            auto record = base_record;
            record.mutable_task()->set_id(id);
            record.mutable_task()->set_submission_sequence(i + 1);
            sqlite::Database::Statement add(
                db,
                "INSERT INTO tasks SELECT ?1, ?2, kind, ?3, state, attempt, updated_at_ms, ?4 " "FROM tasks WHERE id='batch-0'");
            add.bind(1, id)
                .bind(2, i + 1)
                .bind(3, manifest.dump())
                .bind_blob(4, record.SerializeAsString())
                .step();
            sqlite::Database::Statement events(
                db,
                "INSERT INTO task_events SELECT ?1, sequence, body " "FROM task_events WHERE task_id='batch-0'");
            events.bind(1, id).step();
        }
        transaction.commit();
    }
    const auto start = std::chrono::steady_clock::now();
    {
        tasks::Store restored(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        const auto list = restored.list();
        EXPECT_EQ(list.tasks_size(), count);
        for (const auto& task : list.tasks()) {
            EXPECT_EQ(task.state(), task::proto::SUCCEEDED);
            EXPECT_EQ(task.total(), static_cast<unsigned>(protocol::max_dataset_bars));
            EXPECT_EQ(task.definition_case(), task::proto::Task::DEFINITION_NOT_SET);
        }
        EXPECT_EQ(restored.get("batch-23").factor().SerializeAsString(), spec.SerializeAsString());
        EXPECT_EQ(
            tasks::factor_result(restored, "batch-23").SerializeAsString(),
            expected.SerializeAsString());
        RecordProperty("completed_payload_bytes", std::to_string(input_bytes * count));
        RecordProperty("single_payload_bytes", std::to_string(input_bytes));
        RecordProperty("completed_task_count", std::to_string(count));
        RecordProperty(
            "restore_and_verify_ms",
            std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count()));
    }
    // Bounded stopped-service reference inspection still fails closed, but its
    // total-I/O budget must not prevent a valid writer from reopening its store.
    EXPECT_THROW(
        tasks::Store::inspect_history_usage(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"},
            std::string(64, 'a')),
        std::invalid_argument);
}

TEST(TaskStore, DurableUpdatesDoNotChangeSubmissionIdentityOrReorderRetries)
{
    TaskDirectory directory;
    auto clock = std::make_shared<TaskClock>();
    task::proto::Task retried;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"},
            clock);
        const auto original = tasks::submit(store, "first", input());
        tasks::submit(store, "second", input());
        clock->value += 1000000000;
        const auto token = store.commit(store.claim("first")).token();
        EXPECT_EQ(store.get("first").updated_at_ms(), clock->value / 1000000);
        clock->value += 1000000000;
        store.commit(store.progress("first", token, 1));
        const auto progress = store.get("first");
        clock->value += 1000000000;
        store.commit(store.progress("first", token, 1)); // Repeated progress is not a new event.
        EXPECT_EQ(store.get("first").updated_at_ms(), progress.updated_at_ms());
        store.commit(store.fail("first", token, "test worker failure"));
        clock->value += 1000000000;
        retried = store.commit(store.retry("first")).task();
        EXPECT_EQ(retried.updated_at_ms(), clock->value / 1000000);
        EXPECT_EQ(retried.SerializeAsString(), store.describe("first").SerializeAsString());
        EXPECT_EQ(retried.submitted_at_ms(), original.submitted_at_ms());
        EXPECT_EQ(retried.submission_sequence(), original.submission_sequence());
        EXPECT_EQ(store.list().tasks(0).id(), "first");
        auto json = protocol::decode_task(retried);
        EXPECT_EQ(json.at("submitted_at_ms"), original.submitted_at_ms());
        EXPECT_EQ(json.at("updated_at_ms"), retried.updated_at_ms());
        auto missing = retried;
        missing.clear_submitted_at_ms();
        EXPECT_THROW(protocol::decode_task(missing), std::invalid_argument);
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"},
        clock);
    EXPECT_EQ(restored.describe("first").SerializeAsString(), retried.SerializeAsString());
}

TEST(TaskStore, RejectsMissingOrDuplicateChronologyWithoutRewritingEvidence)
{
    for (const auto& mode : {"missing", "duplicate", "gap", "negative", "overflow"}) {
        SCOPED_TRACE(mode);
        TaskDirectory directory;
        {
            tasks::Store store(
                asterion::research::task_domain(),
                directory.path,
                tasks::Identity{"task"});
            tasks::submit(store, "first", input());
            tasks::submit(store, "second", input());
        }
        const auto manifest = [&] {
            sqlite::Database database(directory.path / "tasks.sqlite");
            sqlite::Database::Statement read(
                database,
                "SELECT manifest FROM tasks WHERE id='second'");
            EXPECT_TRUE(read.step());
            return Json::parse(read.text(0));
        };
        auto record = manifest();
        const std::string kind = mode;
        if (kind == "missing")
            record.erase("submitted_at_ms");
        else if (kind == "duplicate")
            record["submission_sequence"] = 1;
        else if (kind == "gap")
            record["submission_sequence"] = 3;
        else if (kind == "negative")
            record["submitted_at_ms"] = -1;
        else
            record["submitted_at_ms"] = std::numeric_limits<std::uint64_t>::max();
        {
            sqlite::Database database(directory.path / "tasks.sqlite");
            sqlite::Database::Statement write(
                database,
                "UPDATE tasks SET manifest=? WHERE id='second'");
            write.bind(1, record.dump()).step();
        }
        EXPECT_THROW(
            tasks::Store rejected(
                asterion::research::task_domain(),
                directory.path,
                tasks::Identity{"task"}),
            std::exception);
        EXPECT_EQ(manifest(), record);
    }
}

TEST(TaskStore, CompletedResultCarriesPersistedExperimentAndRejectsMismatchedEvidence)
{
    TaskDirectory directory;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "evidence", input());
        store.commit(
            tasks::finish(
                store,
                "evidence",
                store.commit(store.claim("evidence")).token(),
                backtest::run(input())));
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    task::proto::TaskResponse response;
    *response.mutable_backtest() = tasks::result(restored, "evidence");
    *response.mutable_result_task() = restored.get("evidence");
    const auto value = protocol::decode_task_result(response, "evidence");
    EXPECT_EQ(
        value.at("experiment").at("strategies").at(0),
        protocol::decode_strategy(testing_support::moving_average(1, 3)));
    EXPECT_EQ(value.at("experiment").at("paper").at("deposit"), "10000");
    EXPECT_EQ(
        value.at("experiment")
            .at("paper")
            .at("contracts")
            .at(0)
            .at("cost_schedule")
            .at(0)
            .at("values")
            .at("close_today_fee"),
        "3");
    EXPECT_EQ(value.at("experiment").at("paper").at("risk").at("max_order_quantity"), "100");
    EXPECT_FALSE(
        value.at("experiment").at("paper").at("contracts").at(0).at("dataset").contains("bars"));
    EXPECT_EQ(value.at("experiment").at("data").at(0).at("count"), 7);
    EXPECT_EQ(value.at("task").at("result_digest"), restored.get("evidence").result_digest());
    EXPECT_THROW(protocol::decode_task_result(response, "other"), std::invalid_argument);
    auto bad = response;
    bad.clear_result_task();
    EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
    bad = response;
    bad.mutable_result_task()->set_state(task::proto::RUNNING);
    EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
    bad = response;
    bad.mutable_backtest()->set_dataset_revision(std::string(64, '0'));
    EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
    bad = response;
    bad.mutable_result_task()->clear_definition();
    EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
    bad = response;
    bad.mutable_result_task()->set_kind(task::proto::FACTOR);
    EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
}

TEST(TaskStore, MultidayEvidenceRestoresAndForgedSettlementCannotCommit)
{
    TaskDirectory root;
    const auto spec = multiday_input();
    const auto expected = backtest::run(spec);
    auto altered = spec;
    altered.mutable_paper()
        ->mutable_contracts(0)
        ->mutable_dataset()
        ->mutable_days(0)
        ->mutable_settlement_price()
        ->set_units(d("107").raw());
    altered.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_revision(
        protocol::bar_dataset_revision(altered.paper().contracts(0).dataset()));
    altered.set_dataset_revision(altered.paper().contracts(0).dataset().revision());
    const auto wrong = backtest::run(altered);
    EXPECT_EQ(wrong.account().equity().units(), expected.account().equity().units());
    {
        tasks::Store store(asterion::research::task_domain(), root.path, tasks::Identity{"task"});
        tasks::submit(store, "multiday", spec);
        const auto token = store.commit(store.claim("multiday")).token();
        EXPECT_THROW(
            store.commit(tasks::finish(store, "multiday", token, wrong)),
            std::invalid_argument);
        auto forged = expected;
        forged.mutable_settlements(0)->mutable_contracts(0)->mutable_price()->set_units(
            d("106").raw());
        EXPECT_THROW(
            store.commit(tasks::finish(store, "multiday", token, forged)),
            std::invalid_argument);
        forged = expected;
        forged.mutable_equity(0)->set_timestamp_ns(expected.equity(0).timestamp_ns() + 1);
        EXPECT_THROW(
            store.commit(tasks::finish(store, "multiday", token, forged)),
            std::invalid_argument);
        store.commit(tasks::finish(store, "multiday", token, expected));
    }
    tasks::Store restored(asterion::research::task_domain(), root.path, tasks::Identity{"task"});
    EXPECT_EQ(
        tasks::result(restored, "multiday").SerializeAsString(),
        expected.SerializeAsString());
    EXPECT_EQ(restored.get("multiday").input().SerializeAsString(), spec.SerializeAsString());
}

TEST(TaskStore, PinnedRiskArtifactIsRetainedButResultReadsDoNotLoadAlgorithm)
{
    TaskDirectory directory;
    std::string artifact;
    const auto expected = backtest::run(input());
    const auto file = risk_providers::Module::filename(directory.path / "done");
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        artifact = tasks::submit(store, "done", input()).risk_artifact();
        EXPECT_EQ(artifact, sha256_file(file));
        store.commit(
            tasks::finish(store, "done", store.commit(store.claim("done")).token(), expected));
        EXPECT_EQ(tasks::result(store, "done").SerializeAsString(), expected.SerializeAsString());
        const auto saved = directory.path / "original";
        std::filesystem::rename(file, saved);
        EXPECT_EQ(tasks::result(store, "done").SerializeAsString(), expected.SerializeAsString());
        EXPECT_THROW(
            risk_providers::Module::pinned(directory.path / "done", artifact),
            std::exception);
        std::ofstream(file) << "corrupt fixture";
        EXPECT_EQ(tasks::result(store, "done").SerializeAsString(), expected.SerializeAsString());
        EXPECT_THROW(
            risk_providers::Module::pinned(directory.path / "done", artifact),
            std::exception);
        EXPECT_EQ(std::filesystem::file_size(file), 15U);
        std::filesystem::remove(file);
        std::filesystem::rename(saved, file);
    }
    tasks::Store recovered(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    EXPECT_EQ(recovered.get("done").risk_artifact(), artifact);
    EXPECT_EQ(tasks::result(recovered, "done").SerializeAsString(), expected.SerializeAsString());
}

TEST(TaskStore, DatedCostsArePinnedSwitchAtSettlementAndRejectForgedResults)
{
    TaskDirectory directory;
    auto spec = multiday_input();
    auto* schedule = spec.mutable_paper()->mutable_contracts(0)->mutable_cost_schedule();
    auto next = schedule->versions(0);
    next.set_effective_from("2026-09-28");
    next.set_source("new published schedule");
    next.mutable_values()->mutable_close_yesterday_fee()->set_units(d("9").raw());
    next.mutable_values()->mutable_margin_per_lot()->set_units(d("700").raw());
    *schedule->add_versions() = next;
    const auto expected = backtest::run(spec);
    EXPECT_EQ(expected.settlements(0).fees().units(), d("2").raw());
    EXPECT_EQ(expected.account().fees().units(), d("16").raw());
    EXPECT_EQ(expected.account().balance().units(), d("10014").raw());
    EXPECT_EQ(expected.account().contracts(0).costs().margin_per_lot().units(), d("700").raw());
    EXPECT_EQ(
        expected.account().contracts(0).cost_schedule().SerializeAsString(),
        schedule->SerializeAsString());
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "dated", spec);
        const auto token = store.commit(store.claim("dated")).token();
        auto forged = expected;
        forged.mutable_account()
            ->mutable_contracts(0)
            ->mutable_cost_schedule()
            ->mutable_versions(1)
            ->set_source("unrecorded replacement");
        EXPECT_THROW(
            store.commit(tasks::finish(store, "dated", token, forged)),
            std::invalid_argument);
        forged = expected;
        forged.mutable_account()
            ->mutable_contracts(0)
            ->mutable_costs()
            ->mutable_margin_per_lot()
            ->set_units(d("100").raw());
        EXPECT_THROW(
            store.commit(tasks::finish(store, "dated", token, forged)),
            std::invalid_argument);
        store.commit(tasks::finish(store, "dated", token, expected));
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    EXPECT_EQ(tasks::result(restored, "dated").SerializeAsString(), expected.SerializeAsString());
    EXPECT_EQ(restored.get("dated").input().SerializeAsString(), spec.SerializeAsString());
}

TEST(TaskStore, MixedHistoricalWarehouseIsRejectedWithoutChangingFiles)
{
    TaskDirectory directory;
    std::filesystem::create_directory(directory.path / "history");
    const auto evidence = directory.path / "history" / "preserve.bin";
    replace_file_durably(evidence, "owned historical data");
    EXPECT_THROW(
        tasks::Store rejected(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"}),
        std::invalid_argument);
    EXPECT_EQ(sha256_file(evidence), sha256_bytes("owned historical data"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "tasks.sqlite"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "manager.lock"));
}

TEST(TaskStore, ForeignStoreIsRejectedBeforeCreatingTaskSchema)
{
    TaskDirectory directory;
    {
        sqlite::Database database(directory.path / "tasks.sqlite");
        database.execute("CREATE TABLE foreign_data(value TEXT)");
        database.execute("INSERT INTO foreign_data VALUES('preserve')");
    }
    const auto before = sha256_file(directory.path / "tasks.sqlite");
    EXPECT_THROW(
        tasks::Store rejected(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"}),
        std::exception);
    EXPECT_EQ(sha256_file(directory.path / "tasks.sqlite"), before);
    EXPECT_FALSE(std::filesystem::exists(directory.path / "manager.lock"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "history"));
    sqlite::Database inspect(directory.path / "tasks.sqlite", sqlite::Database::Access::read_only);
    sqlite::Database::Statement tables(
        inspect,
        "SELECT COUNT(*) FROM sqlite_schema WHERE type='table'");
    ASSERT_TRUE(tables.step());
    EXPECT_EQ(tables.integer(0), 1);
}

TEST(TaskStore, RetainedExperimentsContinueInTheSameWarehouseBeyondOneThousandTasks)
{
    TaskDirectory directory, warehouse;
    std::vector<int> prices(40);
    for (std::size_t i = 0; i < prices.size(); ++i)
        prices[i] = 100 + static_cast<int>(i) + static_cast<int>(i % 3);
    const auto selection = test::seed_history(warehouse.path, prices, "long-lived");
    data::Store data_store(warehouse.path, "historical-data");
    const auto source = data::resolve_bar_dataset(
        data_store.tables(),
        data_store.sources(composed_request({selection})));
    factor::proto::FactorInput spec;
    spec.set_factor(protocol::proto::PRICE_MOMENTUM);
    *factor_bars(spec) = source;
    spec.set_dataset_revision(source.revision());
    spec.add_lookbacks(1);
    spec.set_horizon(1);
    spec.set_full_sample(true);
    const auto expected = factor::run(spec);
    const auto dataset_id = selection.at("source_dataset_ids").at(0).get<std::string>();
    std::string original_hash;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        for (unsigned i = 0; i < 1001; ++i) {
            const auto id = "experiment-" + std::to_string(i);
            const auto submitted = tasks::submit(store, id, spec);
            ASSERT_EQ(submitted.submission_sequence(), i + 1);
            store.commit(tasks::finish(store, id, store.commit(store.claim(id)).token(), expected));
        }
        const auto page = store.list();
        EXPECT_EQ(page.capacity().retained_tasks(), 1001);
        EXPECT_EQ(page.capacity().active_used(), 0);
        EXPECT_EQ(page.tasks_size(), 200);
        EXPECT_EQ(page.tasks(199).submission_sequence(), 1001);
        EXPECT_NO_THROW(protocol::decode_task(page.tasks(199)));
        original_hash = sha256_file(directory.path / "experiment-0/results/1.pb");
        auto usage = store.prepare_history_usage(dataset_id);
        EXPECT_EQ(tasks::submit(store, "experiment-0", spec).state(), task::proto::SUCCEEDED);
        tasks::submit(store, "retry-original", spec);
        store.commit(store.fail(
            "retry-original",
            store.commit(store.claim("retry-original")).token(),
            "fixture failure"));
        unsigned pages = 0;
        while (store.next_history_page(usage)) {
            ++pages;
            auto reading = std::async(std::launch::async, [&] {
                usage.load_page();
            });
            EXPECT_EQ(store.describe("retry-original").state(), task::proto::FAILED);
            reading.get();
        }
        EXPECT_EQ(pages, 6U);
        EXPECT_EQ(usage.take().references_size(), 1001);
    }
    {
        tasks::Store restored(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        EXPECT_EQ(
            tasks::factor_result(restored, "experiment-0").SerializeAsString(),
            expected.SerializeAsString());
        EXPECT_EQ(
            tasks::factor_result(restored, "experiment-1000").SerializeAsString(),
            expected.SerializeAsString());
        EXPECT_EQ(sha256_file(directory.path / "experiment-0/results/1.pb"), original_hash);
        EXPECT_EQ(restored.history_usage(dataset_id).references_size(), 1002);
        const auto retried = restored.commit(restored.retry("retry-original")).task();
        EXPECT_EQ(retried.submission_sequence(), 1002);
        EXPECT_EQ(retried.attempt(), 1);
        EXPECT_EQ(restored.list().capacity().active_used(), 1);
        restored.commit(
            tasks::finish(
                restored,
                "retry-original",
                restored.commit(restored.claim("retry-original")).token(),
                expected));
        EXPECT_EQ(restored.describe("retry-original").attempt(), 2);
        EXPECT_EQ(restored.list().capacity().active_used(), 0);
        EXPECT_EQ(
            data::resolve_bar_dataset(
                data_store.tables(),
                data_store.sources(composed_request({selection})))
                .SerializeAsString(),
            source.SerializeAsString());
    }
    // Membership is part of the recovery index, never authority to silently skip history.
    {
        sqlite::Database database(directory.path / "tasks.sqlite");
        database.execute("DELETE FROM task_event_members WHERE task_id='experiment-0'");
    }
    const auto evidence = sha256_file(directory.path / "tasks.sqlite");
    EXPECT_THROW(
        (tasks::Store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"})),
        std::invalid_argument);
    EXPECT_EQ(sha256_file(directory.path / "tasks.sqlite"), evidence);
}

TEST(TaskStore, ActiveCapacityAppliesToNewAdmissionsAndExplicitRetries)
{
    TaskDirectory directory;
    factor::proto::FactorInput spec;
    spec.set_factor(protocol::proto::PRICE_MOMENTUM);
    *factor_bars(spec) = input().paper().contracts(0).dataset();
    auto* data = factor_bars(spec);
    const auto first = data->bars(0);
    data->clear_bars();
    for (std::int64_t i = 0; i < 40; ++i) {
        auto* bar = data->add_bars();
        *bar = first;
        bar->set_timestamp_ns(first.timestamp_ns() + i * 1000000000);
    }
    data->set_revision(protocol::bar_dataset_revision(*data));
    spec.set_dataset_revision(spec.series(0).bars().revision());
    spec.add_lookbacks(1);
    spec.set_horizon(1);
    spec.set_full_sample(true);
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    for (unsigned i = 0; i < 1000; ++i)
        tasks::submit(store, "queued-" + std::to_string(i), spec);
    EXPECT_EQ(store.list().capacity().active_used(), 1000);
    EXPECT_THROW(tasks::submit(store, "next", spec), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(directory.path / "next"));
    EXPECT_EQ(store.commit(store.cancel("queued-0")).task().state(), task::proto::CANCELLED);
    EXPECT_EQ(store.list().capacity().active_used(), 999);
    auto pending = task_submission(store, "next", spec);
    store.admit_submission(pending);
    EXPECT_EQ(store.list().capacity().active_reserved(), 1U);
    EXPECT_FALSE(std::filesystem::exists(directory.path / "next"));
    EXPECT_THROW(store.commit(store.retry("queued-0")), std::invalid_argument);
    pending.prepare_files();
    EXPECT_EQ(store.commit(store.register_submission(pending)).task().submission_sequence(), 1001);
    EXPECT_EQ(store.list().capacity().active_reserved(), 0U);
    EXPECT_THROW(store.commit(store.retry("queued-0")).task(), std::invalid_argument);
    EXPECT_EQ(store.describe("queued-0").state(), task::proto::CANCELLED);
    store.commit(store.cancel("next")).task();
    EXPECT_EQ(store.commit(store.retry("queued-0")).task().submission_sequence(), 1);
    EXPECT_EQ(store.list().capacity().retained_tasks(), 1001);
    EXPECT_EQ(store.list().capacity().active_used(), 1000);
}

TEST(TaskStore, AComparisonCountsEveryReplayAndKeepsItsScores)
{
    TaskDirectory directory;
    const auto spec = comparison();
    const auto result = backtest::run(spec);
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        const auto task = tasks::submit(store, "compared", spec);
        EXPECT_EQ(task.total(), 80U);
        const auto token = store.commit(store.claim("compared")).token();
        // A result without the scores of what it compared is not this task's.
        auto bare = result;
        bare.clear_candidates();
        bare.clear_selected();
        EXPECT_THROW(
            store.commit(tasks::finish(store, "compared", token, bare)),
            std::invalid_argument);
        // Nor is one that names another strategy than the best of them.
        auto misnamed = result;
        misnamed.set_selected(0);
        EXPECT_THROW(
            store.commit(tasks::finish(store, "compared", token, misnamed)),
            std::invalid_argument);
        store.commit(tasks::finish(store, "compared", token, result));
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    EXPECT_EQ(restored.get("compared").state(), task::proto::SUCCEEDED);
}

TEST(TaskStore, ARollingComparisonKeepsItsRoundsAndRefusesOthers)
{
    TaskDirectory directory;
    const auto spec = rolling_comparison();
    const auto result = backtest::run(spec);
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        const auto task = tasks::submit(store, "rolled", spec);
        EXPECT_EQ(task.total(), 180U);
        const auto token = store.commit(store.claim("rolled")).token();
        // A result without its rounds, or short of one, is not this task's.
        auto bare = result;
        bare.clear_folds();
        EXPECT_THROW(
            store.commit(tasks::finish(store, "rolled", token, bare)),
            std::invalid_argument);
        auto short_of = result;
        short_of.mutable_folds()->RemoveLast();
        EXPECT_THROW(
            store.commit(tasks::finish(store, "rolled", token, short_of)),
            std::invalid_argument);
        // Nor is one whose round followed another strategy than its best, or
        // that begins on another day.
        auto misnamed = result;
        misnamed.mutable_folds(0)->set_selected(1 - result.folds(0).selected());
        EXPECT_THROW(
            store.commit(tasks::finish(store, "rolled", token, misnamed)),
            std::invalid_argument);
        auto moved = result;
        moved.mutable_folds(1)->set_first_day(result.folds(2).first_day());
        EXPECT_THROW(
            store.commit(tasks::finish(store, "rolled", token, moved)),
            std::invalid_argument);
        for (const auto corrupted : {0, 1, 2, 3, 4}) {
            auto changed = result;
            auto& fold = *changed.mutable_folds(0);
            if (corrupted == 0)
                fold.clear_training_first_day();
            else if (corrupted == 1)
                fold.set_training_last_day(fold.first_day());
            else if (corrupted == 2)
                fold.set_last_day(fold.first_day());
            else if (corrupted == 3)
                fold.set_training_days(fold.training_days() - 1);
            else
                fold.set_validation_days(fold.validation_days() + 1);
            EXPECT_THROW(
                store.commit(tasks::finish(store, "rolled", token, changed)),
                std::invalid_argument);
        }
        store.commit(tasks::finish(store, "rolled", token, result));
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    EXPECT_EQ(restored.get("rolled").state(), task::proto::SUCCEEDED);
}

TEST(TaskStore, FailureCodeRemainsAnOptionalObservationAcrossRestartAndExplicitRetry)
{
    TaskDirectory directory;
    const auto spec = input();
    std::string original;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "typed-failure", spec);
        original = store.get("typed-failure").input().SerializeAsString();
        const auto token = store.commit(store.claim("typed-failure")).token();
        store.commit(
            store.fail("typed-failure", token, "fixture source unavailable", "unavailable"));
        tasks::submit(store, "unknown-failure", spec);
        const auto other = store.commit(store.claim("unknown-failure")).token();
        // The same words in a diagnostic are not structured failure evidence.
        store.commit(store.fail("unknown-failure", other, "fixture source unavailable"));
    }
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        EXPECT_EQ(store.get("typed-failure").error_code(), "unavailable");
        EXPECT_TRUE(store.get("unknown-failure").error_code().empty());
        EXPECT_EQ(store.list().tasks(0).error_code(), "unavailable");
        const auto queued = store.commit(store.retry("typed-failure")).task();
        EXPECT_EQ(queued.state(), task::proto::QUEUED);
        EXPECT_TRUE(queued.error_code().empty());
        EXPECT_EQ(store.get("typed-failure").input().SerializeAsString(), original);
    }
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        EXPECT_TRUE(store.get("typed-failure").error_code().empty());
        EXPECT_EQ(store.get("typed-failure").state(), task::proto::QUEUED);
    }
}

TEST(TaskStore, RefreshTargetsAndResultsSurviveRecoveryWithoutChangingEvidence)
{
    TaskDirectory directory;
    auto authorization = refresh_authorization("completed-refresh");
    std::string completed, failed, result;
    {
        tasks::Store store(
            asterion::data::task_domain(),
            directory.path,
            tasks::Identity{"historical-data"});
        tasks::submit(store, authorization);
        const auto token = store.commit(store.claim("completed-refresh")).token();
        task::proto::TaskFinish finish;
        finish.set_id("completed-refresh");
        finish.set_token(token);
        finish.mutable_table_update()->set_unpublished(1);
        store.commit(tasks::finish(store, finish));
        completed = store.get("completed-refresh").SerializeAsString();
        result = tasks::verified_result(store, "completed-refresh").SerializeAsString();
        authorization.set_task_id("failed-refresh");
        authorization.set_id(std::string(64, 'e'));
        tasks::submit(store, authorization);
        const auto other = store.commit(store.claim("failed-refresh")).token();
        store.commit(store.fail("failed-refresh", other, "fixture unavailable", "unavailable"));
        failed = store.get("failed-refresh").SerializeAsString();
    }
    {
        tasks::Store store(
            asterion::data::task_domain(),
            directory.path,
            tasks::Identity{"historical-data"});
        EXPECT_EQ(store.get("completed-refresh").SerializeAsString(), completed);
        EXPECT_EQ(store.get("failed-refresh").SerializeAsString(), failed);
        EXPECT_EQ(tasks::verified_result(store, "completed-refresh").SerializeAsString(), result);
        ASSERT_EQ(store.list().tasks_size(), 2);
        EXPECT_TRUE(store.list().tasks(0).has_table_refresh());
        EXPECT_EQ(
            store.list().tasks(0).table_refresh().SerializeAsString(),
            authorization.table_update().refresh().SerializeAsString());
        const auto retried = store.commit(store.retry("failed-refresh")).task();
        EXPECT_TRUE(retried.has_table_refresh());
        EXPECT_EQ(retried.state(), task::proto::QUEUED);
    }
}

TEST(TaskStore, RefreshReceiptsMatchFixedTargetsAndAttemptIdentityAtCompletionAndRead)
{
    for (const bool unchanged : {false, true}) {
        SCOPED_TRACE(unchanged);
        TaskDirectory directory;
        const auto authorization = refresh_authorization("refresh");
        auto owned_store = std::make_unique<tasks::Store>(
            asterion::data::task_domain(),
            directory.path,
            tasks::Identity{"historical-data"});
        auto& store = *owned_store;
        tasks::submit(store, authorization);
        const auto token = store.commit(store.claim("refresh")).token();
        task::proto::TaskFinish finish;
        finish.set_id("refresh");
        finish.set_token(token);
        auto& result = *finish.mutable_table_update();
        result.set_stored(unchanged ? 0 : 1);
        result.set_unchanged(unchanged ? 1 : 0);
        result.set_snapshot(std::string(64, 'e'));
        auto& receipt = *result.mutable_refreshed_revision();
        receipt.set_sequence(1);
        receipt.set_table("futures.daily");
        receipt.set_schema(1);
        receipt.set_source("fixture.daily.v1");
        receipt.set_exchange("SHFE");
        receipt.set_period("2024-03-01");
        receipt.set_rows(1);
        receipt.set_bytes(10);
        receipt.set_acquired_at_ns(1);
        receipt.set_content(std::string(64, 'e'));
        receipt.set_object(std::string(64, 'f'));
        receipt.set_snapshot(result.snapshot());
        // Unchanged content can have been stored by an earlier provider artifact.
        receipt.set_provider_artifact(std::string(64, unchanged ? '9' : 'b'));
        auto& observation = *result.mutable_refresh_observation();
        observation.set_id(std::string(64, 'a'));
        observation.set_operation(std::string(64, 'b'));
        observation.set_data_instance("historical-data");
        observation.set_task_instance("historical-data");
        observation.set_task_id("refresh");
        observation.set_attempt(1);
        observation.set_provider(authorization.table_update().provider());
        observation.set_provider_artifact(authorization.provider_artifact());
        observation.set_selected_snapshot(
            authorization.table_update().refresh().previous_snapshot());
        observation.set_selected_content(authorization.table_update().refresh().previous_content());
        *observation.mutable_revision() = receipt;
        observation.set_unchanged(unchanged);
        observation.mutable_response()->set_format("tushare.fut_daily.raw.v1");
        observation.mutable_response()->set_body("fixture response");
        observation.mutable_response()->set_received_at_ns(2);
        observation.set_recorded_at_ns(3);
        observation.set_response_bytes(observation.response().body().size());
        observation.set_response_sha256(sha256_bytes(observation.response().body()));
        auto wrong = finish;
        wrong.mutable_table_update()->mutable_refreshed_revision()->set_exchange("DCE");
        wrong.mutable_table_update()->mutable_refreshed_revision()->set_period("2024-04-01");
        EXPECT_THROW(tasks::finish(store, wrong), std::invalid_argument);
        wrong = finish;
        wrong.mutable_table_update()->mutable_refresh_observation()->set_attempt(2);
        EXPECT_THROW(tasks::finish(store, wrong), std::invalid_argument);
        wrong = finish;
        wrong.mutable_table_update()->mutable_refresh_observation()->set_provider_artifact(
            std::string(64, '8'));
        EXPECT_THROW(tasks::finish(store, wrong), std::invalid_argument);
        wrong = finish;
        wrong.mutable_table_update()->clear_refreshed_revision();
        wrong.mutable_table_update()->clear_refresh_observation();
        EXPECT_THROW(tasks::finish(store, wrong), std::invalid_argument);
        EXPECT_EQ(store.describe("refresh").state(), task::proto::RUNNING);
        EXPECT_FALSE(std::filesystem::exists(directory.path / "refresh/results/1.pb"));
        store.commit(tasks::finish(store, finish));
        EXPECT_EQ(
            tasks::verified_result(store, "refresh").table_update().SerializeAsString(),
            result.SerializeAsString());
    }
}

TEST(TaskConfiguration, ReadsPolicyWithoutOverwritingMissingOrInvalidFiles)
{
    TaskDirectory directory;
    const auto file = directory.path / "research.json";
    EXPECT_THROW(tasks::load_task_concurrency(file), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(file));
    replace_file_durably(file, R"({"task_concurrency":2})");
    EXPECT_EQ(tasks::load_task_concurrency(file), 2U);
    const std::string invalid = R"({"task_concurrency":0})";
    replace_file_durably(file, invalid);
    try {
        tasks::load_task_concurrency(file);
        FAIL() << "invalid concurrency accepted";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find(file.string()), std::string::npos);
    }
    std::ifstream input(file);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), invalid);
}

TEST(TaskPreparation, ExplicitResumeReusesOwnedInputAndAllowsIndependentAcceptedWork)
{
    TaskDirectory directory;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        auto draft = task_submission(store, "pending", input());
        store.admit_submission(draft);
        draft.prepare_files(); // Stop before the task index commit.
        EXPECT_EQ(store.list().tasks_size(), 0);
    }
    const auto payload = directory.path / "pending/input.pb";
    const auto before = sha256_file(payload);
    tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
    EXPECT_EQ(restored.list().capacity().uncommitted(), 1U);
    tasks::submit(restored, "independent", input());
    auto draft = restored.resume_submission("pending");
    restored.admit_submission(draft);
    const auto committed = restored.commit(restored.register_submission(draft)).task();
    EXPECT_EQ(committed.id(), "pending");
    EXPECT_EQ(committed.submission_sequence(), 2U);
    EXPECT_EQ(restored.list().capacity().uncommitted(), 0U);
    EXPECT_EQ(restored.list().capacity().retained_tasks(), 2U);
    EXPECT_EQ(sha256_file(payload), before);
    EXPECT_EQ(restored.get("pending").input().SerializeAsString(), input().SerializeAsString());
    auto repeated = restored.resume_submission("pending");
    restored.admit_submission(repeated);
    EXPECT_FALSE(restored.register_submission(repeated).needs_write());
}

TEST(TaskPreparation, MissingPayloadKeepsRecoveryMetadataAndUnknownOrChangedRecordsAreRejected)
{
    TaskDirectory directory;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        auto draft = task_submission(store, "pending", input());
        store.admit_submission(draft);
        draft.prepare_files();
    }
    const auto record_file = directory.path / "pending/prepared.pb";
    const auto payload = directory.path / "pending/input.pb";
    const auto digest = sha256_file(payload);
    std::filesystem::remove(
        payload); // Isolated simulation: stopped before atomic payload publication.
    tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
    const auto prepared = restored.prepared_submission_record("pending");
    EXPECT_EQ(prepared.record().input_digest(), digest);
    EXPECT_EQ(prepared.execution().input_sha256(), digest);
    EXPECT_TRUE(prepared.execution().has_backtest());
    EXPECT_THROW(restored.resume_submission("pending"), std::invalid_argument);
    std::filesystem::create_directory(directory.path / "unknown");
    EXPECT_THROW(restored.prepared_submission_record("unknown"), std::invalid_argument);
    EXPECT_THROW(restored.prepared_submission_record("."), std::invalid_argument);
    EXPECT_THROW(restored.prepared_submission_record(".."), std::invalid_argument);
    auto wrong = prepared;
    wrong.set_instance_id("another-owner");
    replace_file_durably(record_file, wrong.SerializeAsString());
    const auto before = sha256_file(record_file);
    EXPECT_THROW(restored.prepared_submission_record("pending"), std::invalid_argument);
    EXPECT_EQ(sha256_file(record_file), before);
    EXPECT_FALSE(std::filesystem::exists(payload));
}

TEST(TaskPreparation, AbandonmentPersistsBeyondPayloadCleanupAndCannotReleaseAcceptedTasks)
{
    TaskDirectory directory;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        auto draft = task_submission(store, "abandoned", input());
        store.admit_submission(draft);
        draft.prepare_files();
    }
    resource::proto::Object released;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        const auto pending = store.prepared_submission_record("abandoned");
        released = prepared_resource(pending);
        auto decision = store.abandon_preparation(pending, released);
        EXPECT_EQ(store.list().capacity().uncommitted(), 1U);
        decision.persist();
        EXPECT_EQ(store.list().capacity().uncommitted(), 1U);
        store.confirm(decision);
        EXPECT_EQ(store.list().capacity().uncommitted(), 0U);
        ASSERT_TRUE(store.abandoned_preparation("abandoned"));
        EXPECT_EQ(
            store.abandoned_preparation("abandoned")->SerializeAsString(),
            released.SerializeAsString());
        EXPECT_FALSE(store.commit(store.abandon_preparation(pending, released)).needs_write());
        auto resurrection = store.resume_submission("abandoned");
        EXPECT_THROW(store.admit_submission(resurrection), Error);
        tasks::submit(store, "accepted", input());
        const auto accepted = store.prepared_submission_record("accepted");
        EXPECT_THROW(store.abandon_preparation(accepted, prepared_resource(accepted)), Error);
        EXPECT_EQ(store.describe("accepted").state(), task::proto::QUEUED);
    }
    const auto inspect_cold = [&](bool payload_present) {
        unsigned accepted = 0;
        unsigned abandoned = 0;
        const auto catalogue = tasks::Store::inspect_backup(
            asterion::research::task_domain(),
            directory.path,
            {"task"},
            [&](const auto& record, const auto&) {
                EXPECT_EQ(record.task().id(), "accepted");
                ++accepted;
            },
            {},
            [&](const auto& evidence, const auto& decision) {
                EXPECT_EQ(evidence.record().task().id(), "abandoned");
                EXPECT_EQ(evidence.execution().input_sha256(), released.version());
                EXPECT_EQ(decision.SerializeAsString(), released.SerializeAsString());
                ++abandoned;
            });
        EXPECT_EQ(accepted, 1U);
        EXPECT_EQ(abandoned, 1U);
        EXPECT_EQ(
            std::ranges::find(catalogue.files, std::filesystem::path("abandoned/input.pb"))
                != catalogue.files.end(),
            payload_present);
    };
    inspect_cold(true);
    std::filesystem::remove_all(
        directory.path / "abandoned"); // Isolated payload cleanup; authority stays in SQLite.
    inspect_cold(false);
    {
        tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
        EXPECT_EQ(restored.list().capacity().uncommitted(), 0U);
        ASSERT_TRUE(restored.abandoned_preparation("abandoned"));
        EXPECT_EQ(
            restored.abandoned_preparation("abandoned")->SerializeAsString(),
            released.SerializeAsString());
        auto reuse = task_submission(restored, "abandoned", input());
        EXPECT_THROW(restored.admit_submission(reuse), Error);
        EXPECT_EQ(restored.describe("accepted").state(), task::proto::QUEUED);
    }
    // A cleanup cannot erase which fixed sample and candidate were actually prepared.
    sqlite::Database database(directory.path / "tasks.sqlite", sqlite::Database::Access::read_only);
    sqlite::Database::Statement history(
        database,
        "SELECT evidence FROM preparation_decisions WHERE id='abandoned'");
    ASSERT_TRUE(history.step());
    task::proto::PreparedTaskSubmission evidence;
    ASSERT_TRUE(evidence.ParseFromString(history.blob(0)));
    EXPECT_EQ(evidence.execution().input_sha256(), released.version());
    EXPECT_TRUE(evidence.execution().has_backtest());
}

TEST(TaskPreparation, FailedAbandonmentCommitCannotAuthorizeRelease)
{
    TaskDirectory directory;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        auto draft = task_submission(store, "pending", input());
        store.admit_submission(draft);
        draft.prepare_files();
    }
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        const auto pending = store.prepared_submission_record("pending");
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(
            store.commit(store.abandon_preparation(pending, prepared_resource(pending))),
            std::runtime_error);
        EXPECT_FALSE(store.abandoned_preparation("pending"));
        EXPECT_EQ(store.list().capacity().uncommitted(), 1U);
    }
    tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
    EXPECT_FALSE(restored.abandoned_preparation("pending"));
    EXPECT_EQ(restored.list().capacity().uncommitted(), 1U);
    const auto pending = restored.prepared_submission_record("pending");
    restored.commit(restored.abandon_preparation(pending, prepared_resource(pending)));
    EXPECT_TRUE(restored.abandoned_preparation("pending"));
    EXPECT_EQ(restored.list().capacity().uncommitted(), 0U);
}

TEST(TaskRetirement, DurableLifecycleKeepsEvidenceAndCannotResumeOrReuseTheIdentity)
{
    TaskDirectory directory;
    resource::proto::Object object;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        tasks::submit(store, "retired-run", input());
        EXPECT_EQ(
            store.retirement("retired-run", "fixture-node").state(),
            task::proto::TaskRetirement::BLOCKED);
        store.commit(store.cancel("retired-run"));
        const auto preview = store.retirement("retired-run", "fixture-node");
        ASSERT_EQ(preview.state(), task::proto::TaskRetirement::AVAILABLE);
        object = preview.object();
        const auto before = sha256_file(directory.path / "retired-run/input.pb");
        auto decision = store.retire(object);
        EXPECT_FALSE(store.describe("retired-run").has_retirement());
        EXPECT_EQ(
            store.retirement("retired-run", "fixture-node").state(),
            task::proto::TaskRetirement::BLOCKED);
        decision.persist();
        store.confirm(decision);
        EXPECT_TRUE(store.describe("retired-run").has_retirement());
        EXPECT_EQ(sha256_file(directory.path / "retired-run/input.pb"), before);
        EXPECT_EQ(
            store.get("retired-run").input().SerializeAsString(),
            input().SerializeAsString());
        EXPECT_FALSE(store.commit(store.retire(object)).needs_write());
        EXPECT_THROW(store.retry("retired-run"), Error);
        auto reuse = task_submission(store, "retired-run", input());
        EXPECT_THROW(store.admit_submission(reuse), Error);
    }
    tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
    EXPECT_EQ(
        restored.retirement("retired-run", "fixture-node").state(),
        task::proto::TaskRetirement::RETIRED);
    EXPECT_EQ(
        restored.describe("retired-run").retirement().SerializeAsString(),
        object.SerializeAsString());
    EXPECT_EQ(restored.describe("retired-run").state(), task::proto::CANCELLED);
}

TEST(TaskRetirement, NewDerivationAndRetirementShareOneAdmissionBoundary)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
    tasks::submit(store, "parent", input());
    const auto token = store.commit(store.claim("parent")).token();
    store.commit(tasks::finish(store, "parent", token, backtest::run(input())));
    const auto object = store.retirement("parent", "fixture-node").object();
    auto child = task_submission(store, "child", input());
    task::proto::TaskDerivation origin;
    origin.set_source_id("parent");
    origin.set_source_result_digest(store.describe("parent").result_digest());
    origin.set_data_choice("fixed");
    child.set_derivation(origin);
    store.admit_submission(child);
    EXPECT_THROW(store.retire(object), Error);
    child.prepare_files();
    store.commit(store.register_submission(child));
    ASSERT_EQ(store.retirement("parent", "fixture-node").consumers_size(), 1);
    EXPECT_THROW(store.retire(object), Error);
    store.commit(store.cancel("child"));
    store.commit(store.retire(store.retirement("child", "fixture-node").object()));
    auto retirement = store.retire(object);
    auto later = task_submission(store, "later", input());
    later.set_derivation(origin);
    EXPECT_THROW(store.admit_submission(later), Error);
    retirement.persist();
    store.confirm(retirement);
    EXPECT_THROW(store.admit_submission(later), Error);
    EXPECT_EQ(store.list().tasks_size(), 2);
}

TEST(TaskRetirement, FailedPersistenceCannotAuthorizeSourceRelease)
{
    TaskDirectory directory;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        tasks::submit(store, "candidate", input());
        store.commit(store.cancel("candidate"));
        const auto object = store.retirement("candidate", "fixture-node").object();
        auto retirement = store.retire(object);
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(retirement.persist(), std::runtime_error);
        sqlite::fail_next_commits_for_testing(0);
        EXPECT_FALSE(store.describe("candidate").has_retirement());
    }
    tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
    EXPECT_EQ(
        restored.retirement("candidate", "fixture-node").state(),
        task::proto::TaskRetirement::AVAILABLE);
}

TEST(TaskInputReference, IdenticalInputUsesOwnedImmutableStorageAndPreparedRecoveryKeepsTheLink)
{
    TaskDirectory directory;
    const auto spec = input();
    std::string parent_digest;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        tasks::submit(store, "reference-parent", spec);
        const auto token = store.commit(store.claim("reference-parent")).token();
        store.commit(tasks::finish(store, "reference-parent", token, backtest::run(spec)));
        parent_digest = store.describe("reference-parent").result_digest();
        auto child = task_submission(store, "reference-child", spec);
        task::proto::TaskDerivation origin;
        origin.set_source_id("reference-parent");
        origin.set_source_result_digest(parent_digest);
        origin.set_data_choice("fixed");
        child.set_derivation(origin);
        store.admit_submission(child);
        child.prepare_files();
        EXPECT_TRUE(
            std::filesystem::equivalent(
                directory.path / "reference-parent/input.pb",
                directory.path / "reference-child/input.pb"));
        EXPECT_EQ(store.list().tasks_size(), 1);
        // Simulated interruption after complete immutable publication, before admission.
    }
    tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
    auto child = restored.resume_submission("reference-child");
    restored.admit_submission(child);
    restored.commit(restored.register_submission(child));
    EXPECT_EQ(restored.list().tasks_size(), 2);
    EXPECT_EQ(restored.describe("reference-child").state(), task::proto::QUEUED);
    EXPECT_EQ(restored.describe("reference-parent").result_digest(), parent_digest);
    EXPECT_TRUE(
        std::filesystem::equivalent(
            directory.path / "reference-parent/input.pb",
            directory.path / "reference-child/input.pb"));
    EXPECT_EQ(
        restored.get("reference-child").input().SerializeAsString(),
        spec.SerializeAsString());
    // Ordinary in-place writers retain their single-link protection. They cannot
    // truncate shared business bytes, and credential helper rules are unchanged.
    const auto before = sha256_file(directory.path / "reference-child/input.pb");
    EXPECT_THROW(
        write_file_durably(directory.path / "reference-parent/input.pb", "damaged"),
        std::runtime_error);
    EXPECT_EQ(sha256_file(directory.path / "reference-child/input.pb"), before);
    EXPECT_EQ(restored.retirement("reference-parent", "fixture-node").consumers_size(), 1);
}

TEST(TaskInputReference, DifferentInputsDoNotAliasAndCorruptSourcesCannotBeRepublished)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
    const auto spec = input();
    tasks::submit(store, "source", spec);
    const auto token = store.commit(store.claim("source")).token();
    store.commit(tasks::finish(store, "source", token, backtest::run(spec)));
    task::proto::TaskDerivation origin;
    origin.set_source_id("source");
    origin.set_source_result_digest(store.describe("source").result_digest());
    origin.set_data_choice("fixed");
    auto changed_spec = spec;
    *changed_spec.mutable_strategies(0) = testing_support::moving_average(1, 3, "2");
    auto changed = task_submission(store, "changed", changed_spec);
    changed.set_derivation(origin);
    store.admit_submission(changed);
    changed.prepare_files();
    store.commit(store.register_submission(changed));
    EXPECT_FALSE(
        std::filesystem::equivalent(
            directory.path / "source/input.pb",
            directory.path / "changed/input.pb"));
    const auto changed_hash = sha256_file(directory.path / "changed/input.pb");
    replace_file_durably(directory.path / "source/input.pb", "corrupt synthetic source");
    auto candidate = task_submission(store, "candidate", spec);
    candidate.set_derivation(origin);
    store.admit_submission(candidate);
    EXPECT_THROW(candidate.prepare_files(), std::invalid_argument);
    EXPECT_TRUE(store.abandon_submission(candidate));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "candidate/input.pb"));
    EXPECT_TRUE(std::filesystem::exists(directory.path / "candidate/prepared.pb"));
    EXPECT_EQ(
        sha256_file(directory.path / "source/input.pb"),
        sha256_bytes("corrupt synthetic source"));
    EXPECT_EQ(sha256_file(directory.path / "changed/input.pb"), changed_hash);
}

TEST(TaskRetirement, DataAcquisitionKeepsAuthorizationAndRetiresDurably)
{
    TaskDirectory directory;
    const auto authorization = refresh_authorization("retired-acquisition");
    std::string digest;
    resource::proto::Object object;
    {
        tasks::Store store(asterion::data::task_domain(), directory.path, {"historical-data"});
        tasks::submit(store, authorization);
        EXPECT_EQ(
            store.retirement(authorization.task_id(), "fixture-node").state(),
            task::proto::TaskRetirement::BLOCKED);
        const auto token = store.commit(store.claim(authorization.task_id())).token();
        task::proto::TaskFinish finish;
        finish.set_id(authorization.task_id());
        finish.set_token(token);
        finish.mutable_table_update()->set_unpublished(1);
        store.commit(tasks::finish(store, finish));
        EXPECT_EQ(
            asterion::data::table_snapshot_holds(store, std::string(64, 'c')),
            std::vector<std::string>{authorization.task_id()});
        const auto preview = store.retirement(authorization.task_id(), "fixture-node");
        ASSERT_EQ(preview.state(), task::proto::TaskRetirement::AVAILABLE);
        object = preview.object();
        EXPECT_EQ(object.owner().domain(), "data");
        EXPECT_EQ(object.kind(), "table_update");
        digest = sha256_file(directory.path / authorization.task_id() / "input.pb");
        store.commit(store.retire(object));
        EXPECT_TRUE(asterion::data::table_snapshot_holds(store, std::string(64, 'c')).empty());
        EXPECT_FALSE(store.commit(store.retire(object)).needs_write());
        EXPECT_THROW((void)store.retry(authorization.task_id()), std::invalid_argument);
        EXPECT_TRUE(tasks::verified_result(store, authorization.task_id()).has_table_update());
    }
    tasks::Store restored(asterion::data::task_domain(), directory.path, {"historical-data"});
    const auto retired = restored.retirement(authorization.task_id(), "fixture-node");
    ASSERT_EQ(retired.state(), task::proto::TaskRetirement::RETIRED);
    EXPECT_EQ(retired.object().SerializeAsString(), object.SerializeAsString());
    EXPECT_EQ(restored.get(authorization.task_id()).download_authorization(), authorization.id());
    EXPECT_EQ(sha256_file(directory.path / authorization.task_id() / "input.pb"), digest);
    EXPECT_EQ(restored.describe(authorization.task_id()).state(), task::proto::SUCCEEDED);
}

TEST(TaskRetirement, ActiveUsageExcludesRetiredRecordsButHistoryKeepsEvidence)
{
    TaskDirectory directory, warehouse;
    const auto seeded = test::seed_history(warehouse.path, {100, 101}, "retired-usage");
    const auto id = seeded.at("source_dataset_ids").at(0).get<std::string>();
    data::Store data_store(warehouse.path, "historical-data");
    const auto data = data::resolve_bar_dataset(
        data_store.tables(),
        data_store.sources(composed_request({seeded})));
    auto backtest = input();
    *backtest.mutable_paper()->mutable_contracts(0)->mutable_dataset() = data;
    backtest.set_dataset_revision(protocol::dataset_revision(backtest.paper()));
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        tasks::submit(store, "usage", backtest);
        store.commit(store.cancel("usage"));
        EXPECT_EQ(store.history_usage(id, true).references_size(), 1);
        store.commit(store.retire(store.retirement("usage", "fixture-node").object()));
        EXPECT_EQ(store.history_usage(id).references_size(), 1);
        EXPECT_EQ(store.history_usage(id, true).references_size(), 0);
        EXPECT_EQ(store.get("usage").input().SerializeAsString(), backtest.SerializeAsString());
    }
    tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
    EXPECT_EQ(restored.history_usage(id).references_size(), 1);
    EXPECT_EQ(restored.history_usage(id, true).references_size(), 0);
}

TEST(TaskRetirement, SnapshotHoldsRejectUncommittedAndCorruptAcquisitionEvidence)
{
    TaskDirectory directory;
    tasks::Store store(asterion::data::task_domain(), directory.path, {"historical-data"});
    const auto authorization = refresh_authorization("held-evidence");
    auto submission = task_submission(store, authorization);
    store.admit_submission(submission);
    submission.prepare_files();
    EXPECT_THROW(asterion::data::table_snapshot_holds(store, std::string(64, 'c')), Error);
    store.commit(store.register_submission(submission));
    EXPECT_EQ(
        asterion::data::table_snapshot_holds(store, std::string(64, 'c')),
        std::vector<std::string>{authorization.task_id()});
    store.commit(store.cancel(authorization.task_id()));
    const auto path = directory.path / authorization.task_id() / "input.pb";
    replace_file_durably(path, "SYNTHETIC_CORRUPT_INPUT");
    EXPECT_THROW(asterion::data::table_snapshot_holds(store, std::string(64, 'c')), std::exception);
    std::ifstream remaining(path, std::ios::binary);
    EXPECT_EQ(
        std::string(std::istreambuf_iterator<char>(remaining), {}),
        "SYNTHETIC_CORRUPT_INPUT");
    EXPECT_FALSE(store.describe(authorization.task_id()).has_retirement());
}
