#include "task_execution_support.hpp"

TEST(ResearchExecution, CapturedProgramCannotChangeBetweenTheTaskProjectionAndLoadedCalculation)
{
    task::proto::TaskAttempt attempt;
    attempt.mutable_task()->set_kind(task::proto::BACKTEST);
    *attempt.mutable_task()->mutable_input() = input();
    attempt.mutable_task()->set_program_artifact(std::string(64, 'a'));
    *attempt.mutable_execution() = protocol::task_execution(
        attempt.task(),
        sha256_bytes(attempt.task().input().SerializeAsString()));
    const auto expected = attempt.execution();
    attempt.mutable_execution()->set_program_artifact(std::string(64, 'b'));
    EXPECT_THROW(tasks::resolve_task_input(attempt, {}), std::invalid_argument);
    *attempt.mutable_execution() = expected;
    TaskDirectory isolated;
    EXPECT_THROW(tasks::execute(attempt, isolated.path, {}, {}, std::string(64, 'b')), Error);
}

TEST_F(TaskProcess, ServiceRunsWithoutSubmittingClientAndResultSurvivesRestart)
{
    submit("run1"); // Every request closes its connection; no UI client remains.
    research::proto::ResearchRequest fixed;
    fixed.mutable_get()->set_id("run1");
    const auto artifact = sha256_file(ASTERION_RESEARCH_PATH);
    EXPECT_EQ(call(fixed).task().program_artifact(), artifact);
    EXPECT_EQ(sha256_file(directory.path / "programs" / artifact), artifact);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "run1")
            .state(),
        task::proto::SUCCEEDED);
    research::proto::ResearchRequest result;
    result.mutable_result()->set_id("run1");
    EXPECT_EQ(
        call(result).backtest().SerializeAsString(),
        backtest::run(process_input()).SerializeAsString());
    research::proto::ResearchRequest list;
    list.mutable_list()->set_limit(200);
    auto response = call(list);
    ASSERT_EQ(response.tasks().tasks_size(), 1);
    EXPECT_FALSE(response.tasks().tasks(0).has_input());
    service.reset();
    start();
    EXPECT_EQ(
        call(result).backtest().SerializeAsString(),
        backtest::run(process_input()).SerializeAsString());
    submit("run1"); // Same task ID after restart remains idempotent.
    EXPECT_EQ(call(list).tasks().tasks_size(), 1);
    EXPECT_EQ(call(result).result_task().program_artifact(), artifact);
}

TEST_F(TaskProcess, QueuedCalculationWaitsForCurrentProcessObservationWithoutBlockingControls)
{
    const auto stale_pid = service->id();
    auto fixed = process_input();
    service.reset();
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        persist(store, "waiting-for-current-process", fixed);
    }
    // Retain the stopped observation until this test explicitly publishes the new PID.
    discovery->research("task", stale_pid, fixture_program_artifact);
    launch(false);
    wait_for_initialization();
    research::proto::ResearchRequest get;
    get.mutable_get()->set_id("waiting-for-current-process");
    const auto pending = call(get).task();
    EXPECT_EQ(pending.state(), task::proto::QUEUED);
    EXPECT_EQ(pending.attempt(), 0U);
    discovery->research("task", service->id(), fixture_program_artifact);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "waiting-for-current-process")
            .state(),
        task::proto::SUCCEEDED);
    EXPECT_EQ(call(get).task().program_artifact(), fixture_program_artifact);
}

TEST_F(TaskProcess, ABacktestOfAProductTradesTheMonthsTheDailyTableNames)
{
    // Rebar over six days of the daily table: October is the most held until
    // the third day, January from the fourth; May never trades.
    const char* days[] =
        {"2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24", "2026-09-25", "2026-09-28"};
    const auto traded = [&](int base, std::vector<int> held) {
        std::vector<test::DailyRow> rows;
        for (int i = 0; i < 6; ++i) {
            const int price = base + 2 * i;
            rows.push_back(
                {days[i],
                    price,
                    held[static_cast<std::size_t>(i)],
                    test::DailyTrade{price, price + 5, price - 5, price, 100}});
        }
        return rows;
    };
    data_service.reset();
    {
        data::Store created(warehouse.path, "historical-data");
    }
    (void)test::seed_daily_table(
        warehouse.path,
        "SHFE/rb/2026-10",
        traded(200, {100, 100, 40, 30, 20, 10}));
    (void)test::seed_daily_table(
        warehouse.path,
        "SHFE/rb/2027-01",
        traded(100, {10, 60, 90, 95, 99, 99}));
    std::vector<test::DailyRow> idle;
    for (const auto* day : days)
        idle.push_back({day, 150, 5});
    const auto table = test::seed_daily_table(warehouse.path, "SHFE/rb/2027-05", idle);
    start_data();
    research::proto::ResearchRequest submit;
    submit.mutable_submit()->set_id("product");
    auto* backtest = submit.mutable_submit()->mutable_backtest();
    auto* product = backtest->add_series()->mutable_product();
    *product->mutable_data() = protocol::encode_product_bars_request(
        {{"venue", "SHFE"},
            {"product", "rb"},
            {"begin_day", days[0]},
            {"end_day", days[5]},
            {"daily", table}});
    *product->mutable_cost_schedule() = input().paper().contracts(0).cost_schedule();
    product->set_slippage_ticks(1);
    *backtest->mutable_deposit() = input().paper().deposit();
    *backtest->mutable_risk() = input().paper().risk();
    *backtest->add_strategies() = input().strategies(0);
    EXPECT_EQ(call(submit).task().state(), task_wire::QUEUED);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "product")
            .state(),
        task::proto::SUCCEEDED);
    research::proto::ResearchRequest get;
    get.mutable_get()->set_id("product");
    const auto task = call(get).task();
    EXPECT_EQ(task.state(), task_wire::SUCCEEDED);
    // The months that trade are the contracts, each with the product's costs;
    // the month nobody traded is not among them.
    const auto& paper = task.input().paper();
    ASSERT_EQ(paper.contracts_size(), 2);
    EXPECT_EQ(paper.contracts(0).dataset().contract().symbol(), "rb2610");
    EXPECT_EQ(paper.contracts(1).dataset().contract().symbol(), "rb2701");
    for (const auto& contract : paper.contracts()) {
        EXPECT_EQ(contract.dataset().interval_minutes(), 0U);
        EXPECT_EQ(contract.dataset().source_dataset_ids_size(), 0);
        EXPECT_EQ(contract.slippage_ticks(), 1U);
        EXPECT_EQ(
            contract.cost_schedule().SerializeAsString(),
            product->cost_schedule().SerializeAsString());
    }
    ASSERT_EQ(task.input().series_size(), 1);
    ASSERT_EQ(task.input().series(0).rolls_size(), 2);
    EXPECT_EQ(task.input().series(0).rolls(1).trading_day(), days[3]);
    // A list names the series once, over all its days, and the snapshot its
    // months were read under once.
    EXPECT_EQ(task.instrument(), "SHFE/rb (rb2610-rb2701)");
    EXPECT_EQ(task.trading_day(), std::string(days[1]) + " / " + days[5]);
    EXPECT_EQ(task.source_name(), table.at("snapshot").get<std::string>());
    research::proto::ResearchRequest result;
    result.mutable_result()->set_id("product");
    // Settled every day from the second, on which the series begins.
    const auto original_result = call(result);
    EXPECT_EQ(original_result.backtest().settlements_size(), 5);
    const auto original_bytes = task.input().SerializeAsString();
    const auto reuse = [&](const std::string& id,
                           bool fresh,
                           const std::string& snapshot,
                           const std::string& digest) {
        research::proto::ResearchRequest request;
        request.mutable_submit()->set_id(id);
        auto* ref = request.mutable_submit()->mutable_backtest_reuse();
        ref->set_source_id("product");
        ref->set_source_result_digest(task.result_digest());
        ref->set_source_input_sha256(digest);
        ref->set_choice(
            fresh ? backtest::proto::BacktestReuse::NEW_SNAPSHOT
                  : backtest::proto::BacktestReuse::FIXED);
        ref->set_snapshot(snapshot);
        return call(request);
    };
    const auto digest = sha256_bytes(original_bytes);
    EXPECT_THROW(reuse("stale", false, "", std::string(64, 'f')), std::runtime_error);
    EXPECT_EQ(reuse("fixed", false, "", digest).task().state(), task_wire::QUEUED);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "fixed")
            .state(),
        task::proto::SUCCEEDED);
    result.mutable_result()->set_id("fixed");
    const auto fixed_result = call(result);
    EXPECT_EQ(fixed_result.result_task().input().SerializeAsString(), original_bytes);
    EXPECT_EQ(
        fixed_result.backtest().SerializeAsString(),
        original_result.backtest().SerializeAsString());
    data_service.reset();
    const auto revised = test::seed_daily_table(
        warehouse.path,
        "SHFE/rb/2027-01",
        traded(100, {10, 200, 90, 95, 99, 99}));
    start_data();
    EXPECT_EQ(
        reuse("revised", true, revised.at("snapshot").get<std::string>(), digest).task().state(),
        task_wire::QUEUED);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "revised")
            .state(),
        task::proto::SUCCEEDED);
    result.mutable_result()->set_id("revised");
    const auto revised_result = call(result);
    const auto& revised_input = revised_result.result_task().input();
    EXPECT_NE(revised_input.dataset_revision(), task.input().dataset_revision());
    ASSERT_EQ(revised_input.series(0).rolls_size(), 2);
    EXPECT_EQ(revised_input.series(0).rolls(0).trading_day(), days[1]);
    EXPECT_EQ(revised_input.series(0).rolls(1).trading_day(), days[2]);
    EXPECT_EQ(
        revised_input.strategies(0).SerializeAsString(),
        task.input().strategies(0).SerializeAsString());
    EXPECT_EQ(revised_input.paper().risk().SerializeAsString(), paper.risk().SerializeAsString());
    for (const auto& contract : revised_input.paper().contracts()) {
        EXPECT_EQ(
            contract.cost_schedule().SerializeAsString(),
            product->cost_schedule().SerializeAsString());
        EXPECT_EQ(contract.slippage_ticks(), 1U);
    }
    get.mutable_get()->set_id("product");
    EXPECT_EQ(call(get).task().input().SerializeAsString(), original_bytes);
    result.mutable_result()->set_id("product");
    EXPECT_EQ(
        call(result).backtest().SerializeAsString(),
        original_result.backtest().SerializeAsString());
}

TEST_F(TaskProcess, ExecutionRejectsDataEvidenceDifferentFromTheAcceptedInput)
{
    auto spec = process_input();
    auto* evidence =
        spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_history_evidence(0);
    evidence->set_acquired_at_ns(evidence->acquired_at_ns() + 1);
    service.reset();
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        persist(store, "different-evidence", spec);
    }
    start();
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "different-evidence")
            .state(),
        task::proto::FAILED);
    research::proto::ResearchRequest get;
    get.mutable_get()->set_id("different-evidence");
    const auto task = call(get).task();
    EXPECT_EQ(task.state(), task_wire::FAILED);
    EXPECT_EQ(task.error(), "calculation input does not match fixed task evidence");
    EXPECT_TRUE(task.result_digest().empty());
}

TEST_F(TaskProcess, InvalidFixedInputsFailTheirAttemptWithoutRequeueing)
{
    const auto spec = process_input();
    service.reset();
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        persist(store, "other-rules", spec);
        persist(store, "other-messages", spec);
    }
    // A digest-matched input with an unknown field is still invalid.
    {
        const auto path = directory.path / "other-messages" / "input.pb";
        std::ifstream stream(path, std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(stream)), {});
        stream.close();
        raw += "\xb8\x3e\x01"; // field 999, a varint
        replace_file_durably(path, raw);
        sqlite::Database db(directory.path / "tasks.sqlite");
        Json manifest;
        task::proto::StoredTaskRecord record;
        {
            sqlite::Database::Statement read(
                db,
                "SELECT manifest, record FROM tasks WHERE id='other-messages'");
            ASSERT_TRUE(read.step());
            manifest = Json::parse(read.text(0));
            ASSERT_TRUE(record.ParseFromString(read.blob(1)));
        }
        manifest["input_sha256"] = sha256_bytes(raw);
        manifest["input_bytes"] = raw.size();
        record.set_input_digest(sha256_bytes(raw));
        record.set_input_bytes(raw.size());
        sqlite::Database::Statement write(
            db,
            "UPDATE tasks SET manifest=?, record=? WHERE id='other-messages'");
        write.bind(1, manifest.dump()).bind_blob(2, record.SerializeAsString()).step();
    }
    // A consistent index can still disagree with the fixed input's work.
    unsigned admitted = 0;
    {
        sqlite::Database db(directory.path / "tasks.sqlite");
        task::proto::StoredTaskRecord record;
        {
            sqlite::Database::Statement read(db, "SELECT record FROM tasks WHERE id='other-rules'");
            ASSERT_TRUE(read.step());
            ASSERT_TRUE(record.ParseFromString(read.blob(0)));
        }
        admitted = record.task().total() + 1;
        record.mutable_task()->set_total(admitted);
        sqlite::Database::Statement write(db, "UPDATE tasks SET record=? WHERE id='other-rules'");
        write.bind_blob(1, record.SerializeAsString()).step();
    }
    // The store agrees with itself, so the service opens it and lists the task
    // with its stored work. The attempt may already have run by this read.
    start();
    research::proto::ResearchRequest get;
    get.mutable_get()->set_id("other-rules");
    EXPECT_EQ(call(get).task().total(), admitted);
    // The invalid attempt fails once and leaves the queue.
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "other-rules")
            .state(),
        task::proto::FAILED);
    const auto refused = call(get).task();
    EXPECT_EQ(refused.state(), task_wire::FAILED);
    EXPECT_EQ(refused.attempt(), 1U);
    EXPECT_EQ(refused.total(), admitted);
    EXPECT_EQ(refused.error(), "task input does not match admitted work");
    // Reference inspection fails explicitly rather than returning a partial answer.
    research::proto::ResearchRequest usage;
    usage.mutable_history_usage()->set_id(
        spec.paper().contracts(0).dataset().source_dataset_ids(0));
    EXPECT_THROW(call(usage), std::runtime_error);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "other-messages")
            .state(),
        task::proto::FAILED);
    // Asking for the task with its input is refused for the same reason; the
    // list, which reads the index, shows how its attempt ended.
    get.mutable_get()->set_id("other-messages");
    EXPECT_THROW(call(get), std::runtime_error);
    research::proto::ResearchRequest list;
    list.mutable_list()->set_limit(200);
    const auto tasks = call(list).tasks().tasks();
    const auto listed = std::ranges::find(tasks, "other-messages", &task_wire::Task::id);
    ASSERT_NE(listed, tasks.end());
    EXPECT_EQ(listed->state(), task_wire::FAILED);
    EXPECT_EQ(listed->error(), "unknown Protobuf field");
    service.reset();
    start();
    get.mutable_get()->set_id("other-rules");
    EXPECT_EQ(call(get).task().SerializeAsString(), refused.SerializeAsString());
}

TEST_F(TaskProcess, ListenerServesRepeatedConnectionsAndRetainsOwnership)
{
    for (int i = 0; i < 5; ++i) {
        research::proto::ResearchRequest request;
        request.mutable_heartbeat();
        EXPECT_TRUE(call(request).has_health());
    }
    EXPECT_THROW(testing_support::LocalListener duplicate(endpoint), Error);
}

// Research knows its node from its launch arguments; its own records must not
// depend on the Agent staying reachable.
TEST_F(TaskProcess, MetadataRequestsDoNotNeedAgentDiscovery)
{
    discovery.reset();
    research::proto::ResearchRequest registration;
    auto& operation = *registration.mutable_logic()->mutable_mutation();
    operation.set_id("register-without-agent");
    auto& rule = *operation.mutable_registration();
    rule.set_id("two-means");
    rule.set_name("Synthetic two means");
    rule.set_purpose("Pure causal logic, not market performance");
    rule.set_fast(1);
    rule.set_slow(3);
    rule.set_input(research::proto::LogicDefinition::COMPLETED_CLOSE);
    rule.set_output(research::proto::LogicDefinition::TARGET_SIGN);
    const auto definition = call(registration).logic().definition();
    EXPECT_EQ(definition.owner().node_id(), "fixture-node");
}

TEST_F(
    TaskProcess,
    LogicTrialsUsePriorFictionalExpectationsWithoutDataAndKeepOriginalAfterDeprecation)
{
    EXPECT_FALSE(data_service);
    research::proto::ResearchRequest registration;
    auto& operation = *registration.mutable_logic()->mutable_mutation();
    operation.set_id("register-rule");
    auto& rule = *operation.mutable_registration();
    rule.set_id("two-means");
    rule.set_name("Synthetic two means");
    rule.set_purpose("Pure causal logic, not market performance");
    rule.set_fast(1);
    rule.set_slow(3);
    rule.set_input(research::proto::LogicDefinition::COMPLETED_CLOSE);
    rule.set_output(research::proto::LogicDefinition::TARGET_SIGN);
    const auto definition = call(registration).logic().definition();
    research::proto::ResearchRequest save;
    auto& mutation = *save.mutable_logic()->mutable_mutation();
    mutation.set_id("save-prior-case");
    auto& example = *mutation.mutable_example();
    example.set_id("prior-case");
    *example.mutable_definition() = protocol::logic_object(definition);
    example.set_name("Prior fictional mismatch");
    example.set_use_scope("Four fictional observations only; no financial conclusion");
    example.set_fictional(true);
    for (int price : {10, 10, 13, 7}) {
        const auto index = example.observations_size();
        auto& row = *example.add_observations();
        row.set_closed_at_ns((index + 1) * 60000000000LL);
        row.set_available_at_ns(row.closed_at_ns());
        row.mutable_close()->set_units(Decimal::parse(std::to_string(price)).raw());
        if (index < 2)
            row.set_warmup(true);
        else
            row.set_target_sign(-1);
    }
    const auto saved = call(save).logic().example();
    EXPECT_EQ(call(save).logic().example().SerializeAsString(), saved.SerializeAsString());
    auto changed = save;
    changed.mutable_logic()
        ->mutable_mutation()
        ->mutable_example()
        ->mutable_observations(2)
        ->set_target_sign(1);
    EXPECT_THROW(call(changed), std::runtime_error);
    research::proto::ResearchRequest submit;
    submit.mutable_submit()->set_id("logic-comparison");
    *submit.mutable_submit()->mutable_logic_trial()->mutable_example() =
        protocol::logic_object(saved);
    call(submit);
    const auto finished = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "logic-comparison");
    ASSERT_EQ(finished.state(), task_wire::SUCCEEDED) << finished.error();
    research::proto::ResearchRequest read;
    read.mutable_result()->set_id("logic-comparison");
    const auto original = call(read);
    ASSERT_TRUE(original.has_logic_trial());
    EXPECT_FALSE(original.logic_trial().all_match());
    EXPECT_EQ(original.logic_trial().matched(), 3U);
    EXPECT_EQ(original.logic_trial().decisions(), 2U);
    EXPECT_EQ(
        original.result_task().logic_trial().example().SerializeAsString(),
        saved.SerializeAsString());
    EXPECT_EQ(original.result_task().program_artifact(), fixture_program_artifact);
    service.reset();
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        auto prepared =
            task_submission(store, "logic-prepared", original.result_task().logic_trial());
        asterion::tasks::set_program_artifact(prepared, fixture_program_artifact);
        store.admit_submission(prepared);
        prepared.prepare_files();
        std::filesystem::remove(directory.path / "logic-prepared/input.pb");
    }
    start();
    research::proto::ResearchRequest recover;
    recover.mutable_recover_submission()->set_id("logic-prepared");
    EXPECT_EQ(call(recover).task().kind(), task_wire::LOGIC_TRIAL);
    const auto recovered = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "logic-prepared");
    ASSERT_EQ(recovered.state(), task_wire::SUCCEEDED) << recovered.error();
    research::proto::ResearchRequest recovered_read;
    recovered_read.mutable_result()->set_id("logic-prepared");
    EXPECT_EQ(
        call(recovered_read).logic_trial().SerializeAsString(),
        original.logic_trial().SerializeAsString());
    service.reset();
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        auto prepared =
            task_submission(store, "logic-abandoned", original.result_task().logic_trial());
        asterion::tasks::set_program_artifact(prepared, fixture_program_artifact);
        store.admit_submission(prepared);
        prepared.prepare_files();
        std::filesystem::remove(directory.path / "logic-abandoned/input.pb");
    }
    start();
    research::proto::ResearchRequest abandon;
    abandon.mutable_abandon_preparation()->set_id("logic-abandoned");
    const auto decision = call(abandon).release_decision();
    EXPECT_EQ(decision.state(), resource::proto::ReleaseDecision::RELEASED);
    EXPECT_EQ(decision.object().kind(), "logic_trial");
    EXPECT_EQ(call(abandon).release_decision().SerializeAsString(), decision.SerializeAsString());
    recover.mutable_recover_submission()->set_id("logic-abandoned");
    EXPECT_THROW(call(recover), std::runtime_error);
    research::proto::ResearchRequest retire;
    auto& removal = *retire.mutable_logic()->mutable_mutation();
    removal.set_id("deprecate-rule");
    *removal.mutable_retire()->mutable_definition() = protocol::logic_object(definition);
    removal.mutable_retire()->set_reason("No new trials; retain original evidence");
    EXPECT_FALSE(call(retire).logic().definition().retirement_reason().empty());
    EXPECT_EQ(call(submit).task().state(), task_wire::SUCCEEDED);
    auto forbidden = submit;
    forbidden.mutable_submit()->set_id("forbidden-trial");
    EXPECT_THROW(call(forbidden), std::runtime_error);
    service.reset();
    start();
    EXPECT_FALSE(data_service);
    const auto reopened = call(read);
    EXPECT_EQ(
        reopened.result_task().SerializeAsString(),
        original.result_task().SerializeAsString());
    EXPECT_EQ(
        reopened.logic_trial().SerializeAsString(),
        original.logic_trial().SerializeAsString());
    EXPECT_EQ(call(submit).task().state(), task_wire::SUCCEEDED);
    EXPECT_EQ(call(save).logic().example().SerializeAsString(), saved.SerializeAsString());
    research::proto::ResearchRequest retirement;
    retirement.mutable_retirement()->set_id("logic-comparison");
    EXPECT_EQ(call(retirement).retirement().state(), task_wire::TaskRetirement::AVAILABLE);
    retirement.mutable_retirement()->set_execute(true);
    const auto retired = call(retirement).retirement();
    EXPECT_EQ(retired.state(), task_wire::TaskRetirement::RETIRED);
    EXPECT_TRUE(retired.source_release_confirmed());
    EXPECT_EQ(
        call(read).logic_trial().SerializeAsString(),
        original.logic_trial().SerializeAsString());
}

TEST_F(
    TaskProcess,
    MatchingLogicPackageRequiresExplicitBacktestConditionsAndKeepsActualUseAfterRestart)
{
    research::proto::ResearchRequest registration;
    auto& operation = *registration.mutable_logic()->mutable_mutation();
    operation.set_id("register-logic");
    auto& rule = *operation.mutable_registration();
    rule.set_id("means");
    rule.set_name("Fictional logic");
    rule.set_purpose("No market performance claim");
    rule.set_fast(1);
    rule.set_slow(3);
    rule.set_input(research::proto::LogicDefinition::COMPLETED_CLOSE);
    rule.set_output(research::proto::LogicDefinition::TARGET_SIGN);
    const auto definition = call(registration).logic().definition();
    research::proto::ResearchRequest save;
    auto& mutation = *save.mutable_logic()->mutable_mutation();
    mutation.set_id("prior-expectations");
    auto& example = *mutation.mutable_example();
    example.set_id("prior-case");
    example.set_name("Fictional rising and falling");
    example.set_use_scope("Pure causal rule check only");
    example.set_fictional(true);
    *example.mutable_definition() = protocol::logic_object(definition);
    for (int price : {10, 10, 13, 7}) {
        const auto index = example.observations_size();
        auto& row = *example.add_observations();
        row.set_closed_at_ns((index + 1) * 60000000000LL);
        row.set_available_at_ns(row.closed_at_ns());
        row.mutable_close()->set_units(Decimal::parse(std::to_string(price)).raw());
        if (index < 2)
            row.set_warmup(true);
        else
            row.set_target_sign(index == 2 ? 1 : -1);
    }
    const auto saved = call(save).logic().example();
    research::proto::ResearchRequest trial;
    trial.mutable_submit()->set_id("matching-logic");
    *trial.mutable_submit()->mutable_logic_trial()->mutable_example() =
        protocol::logic_object(saved);
    call(trial);
    ASSERT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "matching-logic")
            .state(),
        task_wire::SUCCEEDED);
    research::proto::ResearchRequest read;
    read.mutable_result()->set_id("matching-logic");
    const auto source = call(read);
    ASSERT_TRUE(source.logic_trial().all_match());
    const auto input = process_input();
    research::proto::ResearchRequest assemble;
    auto& frozen = *assemble.mutable_strategy()->mutable_mutation();
    frozen.set_id("assemble-logic");
    auto& package = *frozen.mutable_logic();
    package.set_id("logic-package");
    package.set_name("Independent rule candidate");
    package.set_purpose("Explicit research backtest");
    package.set_limitations("Fictional checking is not return evidence or live authorization");
    auto& reference = *package.mutable_source();
    *reference.mutable_owner() = saved.owner();
    reference.set_kind("logic_trial_result");
    reference.set_id("matching-logic");
    reference.set_version(source.result_task().result_digest());
    *package.mutable_contract() = input.paper().contracts(0).dataset().contract();
    package.set_interval_minutes(1);
    *package.mutable_strategy() = protocol::encode_strategy(
        {{"quantity", "1"},
            {"sides", "both"},
            {"rule", {{"kind", "moving_average"}, {"fast", 1}, {"slow", 3}}}});
    const auto candidate = call(assemble).strategy().package();
    ASSERT_TRUE(candidate.has_logic());
    EXPECT_FALSE(candidate.has_reference_result());
    research::proto::ResearchRequest deliver;
    auto& draft_request = *deliver.mutable_strategy()->mutable_mutation();
    draft_request.set_id("save-draft");
    draft_request.mutable_deliver()->set_id("logic-draft");
    draft_request.mutable_deliver()->set_target_task_id("logic-backtest");
    *draft_request.mutable_deliver()->mutable_package() =
        protocol::strategy_package_object(candidate);
    const auto draft = call(deliver).strategy().delivery();
    research::proto::ResearchRequest lookup;
    lookup.mutable_strategy()->mutable_delivery()->set_id(draft.id());
    EXPECT_FALSE(call(lookup).strategy().target_accepted());
    research::proto::ResearchRequest retirement;
    retirement.mutable_retirement()->set_id("matching-logic");
    EXPECT_EQ(call(retirement).retirement().state(), task_wire::TaskRetirement::BLOCKED);
    research::proto::ResearchRequest accept;
    auto& submission = *accept.mutable_submit();
    submission.set_id(draft.target_task_id());
    auto& spec = *submission.mutable_backtest();
    auto& contract = *spec.add_contracts();
    *contract.mutable_data() =
        protocol::bar_dataset_selection(input.paper().contracts(0).dataset());
    *contract.mutable_cost_schedule() = input.paper().contracts(0).cost_schedule();
    contract.set_slippage_ticks(input.paper().contracts(0).slippage_ticks());
    *spec.mutable_deposit() = input.paper().deposit();
    *spec.mutable_risk() = input.paper().risk();
    *spec.add_strategies() = candidate.definition().strategy();
    *submission.mutable_strategy_use()->mutable_package() =
        protocol::strategy_package_object(candidate);
    *submission.mutable_strategy_use()->mutable_delivery() =
        protocol::strategy_delivery_object(draft);
    auto missing_risk = accept;
    missing_risk.mutable_submit()->mutable_backtest()->clear_risk();
    EXPECT_THROW(call(missing_risk), std::runtime_error);
    auto wrong_rule = accept;
    wrong_rule.mutable_submit()
        ->mutable_backtest()
        ->mutable_strategies(0)
        ->mutable_moving_average()
        ->set_fast(2);
    EXPECT_THROW(call(wrong_rule), std::runtime_error);
    call(accept);
    const auto completed = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        draft.target_task_id());
    ASSERT_EQ(completed.state(), task_wire::SUCCEEDED) << completed.error();
    research::proto::ResearchRequest report;
    report.mutable_result()->set_id(draft.target_task_id());
    const auto actual = call(report);
    EXPECT_EQ(
        actual.result_task().strategy_use().request_sha256(),
        sha256_bytes(spec.SerializeAsString()));
    EXPECT_EQ(
        actual.result_task().strategy_use().package().SerializeAsString(),
        submission.strategy_use().package().SerializeAsString());
    EXPECT_EQ(
        actual.result_task().input().strategies(0).SerializeAsString(),
        candidate.definition().strategy().SerializeAsString());
    EXPECT_TRUE(call(lookup).strategy().target_accepted());
    research::proto::ResearchRequest deprecate;
    auto& removal = *deprecate.mutable_strategy()->mutable_mutation();
    removal.set_id("deprecate-package");
    *removal.mutable_retire()->mutable_package() = protocol::strategy_package_object(candidate);
    removal.mutable_retire()->set_reason("No new use; keep actual admitted source");
    call(deprecate);
    const auto held = call(retirement).retirement();
    EXPECT_EQ(held.state(), task_wire::TaskRetirement::BLOCKED);
    EXPECT_NE(std::ranges::find(held.consumers(), draft.target_task_id()), held.consumers().end());
    service.reset();
    data_service.reset();
    start();
    EXPECT_EQ(call(accept).task().state(), task_wire::SUCCEEDED);

    auto changed = accept;
    changed.mutable_submit()->mutable_backtest()->mutable_deposit()->set_units(
        Decimal::parse("200000").raw());
    EXPECT_THROW(call(changed), std::runtime_error);
    EXPECT_EQ(call(report).backtest().SerializeAsString(), actual.backtest().SerializeAsString());
    EXPECT_EQ(
        call(report).result_task().strategy_use().SerializeAsString(),
        actual.result_task().strategy_use().SerializeAsString());
    EXPECT_TRUE(call(lookup).strategy().target_accepted());
}

TEST_F(TaskProcess, StrategyCandidateDraftAndSourceHoldPersistAcrossResearchRestart)
{
    const auto original = process_input();
    research::proto::ResearchRequest create;
    create.mutable_submit()->set_id("strategy-reference");
    auto& spec = *create.mutable_submit()->mutable_backtest();
    const auto& contract = original.paper().contracts(0);
    auto& source_contract = *spec.add_contracts();
    *source_contract.mutable_data() = protocol::bar_dataset_selection(contract.dataset());
    *source_contract.mutable_cost_schedule() = contract.cost_schedule();
    source_contract.set_slippage_ticks(contract.slippage_ticks());
    *spec.mutable_deposit() = original.paper().deposit();
    *spec.mutable_risk() = original.paper().risk();
    *spec.mutable_strategies() = original.strategies();
    ASSERT_EQ(call(create).task().state(), task_wire::QUEUED);
    const auto reference_state = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "strategy-reference");
    ASSERT_EQ(reference_state.state(), task_wire::SUCCEEDED) << reference_state.error();
    research::proto::ResearchRequest result;
    result.mutable_result()->set_id("strategy-reference");
    const auto evidence = call(result);
    research::proto::ResearchRequest freeze;
    auto& mutation = *freeze.mutable_strategy()->mutable_mutation();
    mutation.set_id("freeze-candidate");
    auto& registration = *mutation.mutable_freeze();
    registration.set_id("candidate");
    registration.set_name("Synthetic single CTA");
    registration.set_purpose("Explicit fixed-input research delivery");
    registration.set_limitations("Synthetic evidence, no predictive or trading authorization");
    auto& source = *registration.mutable_source();
    source.mutable_owner()->set_node_id("fixture-node");
    source.mutable_owner()->set_service_id("task");
    source.mutable_owner()->set_domain("research");
    source.set_kind("backtest_result");
    source.set_id("strategy-reference");
    source.set_version(evidence.result_task().result_digest());
    const auto candidate = call(freeze).strategy().package();
    EXPECT_EQ(
        candidate.source_input_sha256(),
        sha256_bytes(evidence.result_task().input().SerializeAsString()));
    EXPECT_EQ(candidate.definition().program_artifact(), fixture_program_artifact);
    EXPECT_EQ(candidate.retirement_reason(), "");
    EXPECT_EQ(call(freeze).strategy().package().SerializeAsString(), candidate.SerializeAsString());
    auto changed = freeze;
    changed.mutable_strategy()->mutable_mutation()->mutable_freeze()->set_purpose("changed");
    EXPECT_THROW(call(changed), std::runtime_error);
    research::proto::ResearchRequest retirement;
    retirement.mutable_retirement()->set_id("strategy-reference");
    retirement.mutable_retirement()->set_execute(true);
    const auto held = call(retirement).retirement();
    ASSERT_EQ(held.state(), task_wire::TaskRetirement::BLOCKED);
    ASSERT_EQ(held.consumers_size(), 1);
    EXPECT_EQ(held.consumers(0), "strategy_package/candidate");
    research::proto::ResearchRequest deliver;
    auto& creation = *deliver.mutable_strategy()->mutable_mutation();
    creation.set_id("create-delivery");
    creation.mutable_deliver()->set_id("delivery");
    creation.mutable_deliver()->set_target_task_id("explicit-future-run");
    *creation.mutable_deliver()->mutable_package() = protocol::strategy_package_object(candidate);
    const auto draft = call(deliver).strategy().delivery();
    research::proto::ResearchRequest lookup;
    lookup.mutable_strategy()->mutable_delivery()->set_id("delivery");
    EXPECT_FALSE(call(lookup).strategy().target_accepted());
    research::proto::ResearchRequest missing;
    missing.mutable_get()->set_id("explicit-future-run");
    EXPECT_THROW(call(missing), std::runtime_error);
    research::proto::ResearchRequest catalogue;
    catalogue.mutable_resources()->set_kind("strategy_package");
    catalogue.mutable_resources()->set_limit(10);
    EXPECT_EQ(call(catalogue).resources().items_size(), 1);
    research::proto::ResearchRequest accept;
    auto& submitted = *accept.mutable_submit();
    submitted.set_id(draft.target_task_id());
    *submitted.mutable_strategy_use()->mutable_package() = draft.package();
    *submitted.mutable_strategy_use()->mutable_delivery() =
        protocol::strategy_delivery_object(draft);
    auto& fixed = *submitted.mutable_backtest_reuse();
    fixed.set_choice(backtest::proto::BacktestReuse::FIXED);
    fixed.set_source_id(draft.reference_result().id());
    fixed.set_source_result_digest(draft.reference_result().version());
    fixed.set_source_input_sha256(draft.source_input_sha256());
    auto altered = accept;
    altered.mutable_submit()->mutable_backtest_reuse()->set_slippage_ticks(2);
    EXPECT_THROW(call(altered), std::runtime_error);
    auto wrong_source = accept;
    wrong_source.mutable_submit()->mutable_backtest_reuse()->set_source_input_sha256(
        std::string(64, 'f'));
    EXPECT_THROW(call(wrong_source), std::runtime_error);
    const auto admitted = call(accept).task();
    EXPECT_EQ(
        admitted.strategy_use().SerializeAsString(),
        submitted.strategy_use().SerializeAsString());
    EXPECT_TRUE(call(lookup).strategy().target_accepted());
    ASSERT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            draft.target_task_id())
            .state(),
        task_wire::SUCCEEDED);
    result.mutable_result()->set_id(draft.target_task_id());
    const auto used = call(result);
    EXPECT_EQ(
        used.result_task().strategy_use().SerializeAsString(),
        submitted.strategy_use().SerializeAsString());
    EXPECT_EQ(
        used.result_task().input().SerializeAsString(),
        evidence.result_task().input().SerializeAsString());
    service.reset();
    data_service.reset();
    start();
    EXPECT_TRUE(call(lookup).strategy().target_accepted());
    EXPECT_EQ(call(accept).task().state(), task_wire::SUCCEEDED);
    EXPECT_EQ(call(freeze).strategy().package().SerializeAsString(), candidate.SerializeAsString());
    EXPECT_EQ(call(lookup).strategy().delivery().SerializeAsString(), draft.SerializeAsString());
    EXPECT_TRUE(call(lookup).strategy().target_accepted());
    EXPECT_EQ(call(retirement).retirement().state(), task_wire::TaskRetirement::BLOCKED);
    research::proto::ResearchRequest deprecate;
    auto& removal = *deprecate.mutable_strategy()->mutable_mutation();
    removal.set_id("deprecate-candidate");
    *removal.mutable_retire()->mutable_package() = protocol::strategy_package_object(candidate);
    removal.mutable_retire()->set_reason("Research candidate withdrawn");
    EXPECT_EQ(
        call(deprecate).strategy().package().retirement_reason(),
        "Research candidate withdrawn");
    retirement.mutable_retirement()->set_execute(false);
    const auto retained = call(retirement).retirement();
    EXPECT_EQ(retained.state(), task_wire::TaskRetirement::BLOCKED);
    ASSERT_EQ(retained.consumers_size(), 1);
    EXPECT_EQ(retained.consumers(0), draft.target_task_id());
    EXPECT_EQ(call(accept).task().state(), task_wire::SUCCEEDED);
    EXPECT_EQ(
        call(result).result_task().strategy_use().SerializeAsString(),
        submitted.strategy_use().SerializeAsString());
    EXPECT_EQ(call(freeze).strategy().package().SerializeAsString(), candidate.SerializeAsString());
    EXPECT_EQ(call(lookup).strategy().delivery().SerializeAsString(), draft.SerializeAsString());
    auto forbidden = deliver;
    auto* next = forbidden.mutable_strategy()->mutable_mutation();
    next->set_id("new-delivery");
    next->mutable_deliver()->set_id("forbidden-delivery");
    next->mutable_deliver()->set_target_task_id("forbidden-run");
    EXPECT_THROW(call(forbidden), std::runtime_error);
}

TEST_F(TaskProcess, CausalVariablesPersistDefinitionsAndActualFactorEvidenceAcrossRestart)
{
    EXPECT_FALSE(data_service);
    research::proto::ResearchRequest preview;
    auto& input = *preview.mutable_variable()->mutable_preview();
    input.set_expression("forward_return");
    input.set_window(2);
    const auto invalid = call(preview).variable().preview();
    EXPECT_FALSE(invalid.valid());
    ASSERT_TRUE(invalid.has_error_offset());
    EXPECT_EQ(invalid.error_offset(), 0U);
    EXPECT_EQ(invalid.diagnostic(), "unknown field or function");
    input.set_expression("close / lag(close,window) - 1");
    const auto checked = call(preview).variable().preview();
    EXPECT_TRUE(checked.valid());
    EXPECT_EQ(checked.required_history(), 2U);
    EXPECT_EQ(checked.price_power(), 0);
    service.reset();
    {
        sqlite::Database database(directory.path / "tasks.sqlite");
        sqlite::Database::Statement tables(
            database,
            "SELECT count(*) FROM sqlite_master WHERE name LIKE 'research_variable_%'");
        ASSERT_TRUE(tables.step());
        EXPECT_EQ(tables.integer(0), 0);
    }
    start();
    research::proto::ResearchRequest registration;
    auto& mutation = *registration.mutable_variable()->mutable_mutation();
    mutation.set_id("variable-operation-one");
    auto& definition = *mutation.mutable_registration();
    definition.set_id("variable-one");
    definition.set_name("A declared variable");
    definition.set_purpose("Research predictor after a completed close, not a future label");
    definition.set_expression(input.expression());
    definition.set_window(2);
    const auto original = call(registration).variable().definition();
    EXPECT_EQ(original.required_history(), 2U);
    EXPECT_EQ(original.owner().node_id(), "fixture-node");
    EXPECT_EQ(original.owner().service_id(), "task");
    auto next = registration;
    next.mutable_variable()->mutable_mutation()->set_id("variable-operation-two");
    next.mutable_variable()->mutable_mutation()->mutable_registration()->set_id("variable-two");
    next.mutable_variable()->mutable_mutation()->mutable_registration()->set_window(3);
    const auto second = call(next).variable().definition();
    EXPECT_NE(second.digest(), original.digest());
    EXPECT_EQ(second.name(), original.name());
    EXPECT_EQ(
        call(registration).variable().definition().SerializeAsString(),
        original.SerializeAsString());
    auto changed = registration;
    changed.mutable_variable()->mutable_mutation()->mutable_registration()->set_window(3);
    EXPECT_THROW(call(changed), std::runtime_error);
    research::proto::ResearchRequest page;
    page.mutable_variable()->mutable_list()->set_limit(1);
    const auto first_page = call(page).variable();
    ASSERT_EQ(first_page.definitions_size(), 1);
    EXPECT_EQ(first_page.definitions(0).id(), "variable-one");
    EXPECT_EQ(first_page.next_after(), "variable-one");
    page.mutable_variable()->mutable_list()->set_after(first_page.next_after());
    const auto second_page = call(page).variable();
    ASSERT_EQ(second_page.definitions_size(), 1);
    EXPECT_EQ(second_page.definitions(0).id(), "variable-two");
    EXPECT_TRUE(second_page.next_after().empty());
    service.reset();
    start();
    research::proto::ResearchRequest get;
    get.mutable_variable()->mutable_get()->set_id(original.id());
    EXPECT_EQ(call(get).variable().definition().SerializeAsString(), original.SerializeAsString());
    EXPECT_EQ(
        call(registration).variable().definition().SerializeAsString(),
        original.SerializeAsString());
    research::proto::ResearchRequest resources;
    resources.mutable_resources()->set_kind("research_variable");
    resources.mutable_resources()->set_limit(10);
    const auto catalogue = call(resources).resources();
    ASSERT_EQ(catalogue.items_size(), 2);
    EXPECT_EQ(catalogue.owner().SerializeAsString(), original.owner().SerializeAsString());
    EXPECT_EQ(catalogue.items(0).object().version(), original.digest());
    EXPECT_FALSE(data_service);
    auto spec = maximum_factor_input();
    std::vector<int> prices(40);
    for (int i = 0; i < 40; ++i)
        prices[static_cast<std::size_t>(i)] = 100 + i + i % 3;
    prices[0] = 100;
    prices[1] = 102;
    prices[2] = 101;
    prices[3] = 103;
    *factor_bars(spec) = published_dataset(prices, "expression-data");
    spec.mutable_data_owner()->set_node_id("fixture-node");
    spec.mutable_data_owner()->set_service_id("historical-data");
    spec.set_dataset_revision(protocol::factor_revision(spec.series()));
    spec.set_factor(protocol::proto::FACTOR_UNSPECIFIED);
    spec.set_retain_observations(true);
    spec.set_horizon(1);
    spec.mutable_lookbacks()->Clear();
    spec.add_lookbacks(original.window());
    *spec.mutable_variable_definition() = original;
    task_wire::Task fixed;
    fixed.set_kind(task_wire::FACTOR);
    *fixed.mutable_factor() = spec;
    research::proto::ResearchRequest submit;
    auto& admission = *submit.mutable_submit();
    admission.set_id("expression-original");
    *admission.mutable_factor_request() = protocol::task_execution(fixed, "").factor();
    *admission.mutable_data_owner() = spec.data_owner();
    const auto admitted = call(submit, {}, 10s).task();
    EXPECT_EQ(
        admitted.variable_definition().SerializeAsString(),
        protocol::variable_object(original).SerializeAsString());
    const auto finished = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "expression-original");
    ASSERT_EQ(finished.state(), task_wire::SUCCEEDED) << finished.error();
    research::proto::ResearchRequest result;
    result.mutable_result()->set_id("expression-original");
    const auto evidence = call(result);
    EXPECT_EQ(
        evidence.result_task().factor().variable_definition().SerializeAsString(),
        original.SerializeAsString());
    EXPECT_EQ(evidence.result_task().program_artifact(), fixture_program_artifact);
    ASSERT_EQ(evidence.factor().observations_size(), 40);
    EXPECT_NEAR(evidence.factor().observations(2).value(), 0.01, 1e-12);
    EXPECT_FALSE(evidence.factor().observations(1).has_value());
    EXPECT_FALSE(evidence.factor().observations(39).has_forward_return());
    auto revision = registration;
    revision.mutable_variable()->mutable_mutation()->set_id("variable-operation-three");
    revision.mutable_variable()->mutable_mutation()->mutable_registration()->set_id(
        "variable-three");
    revision.mutable_variable()->mutable_mutation()->mutable_registration()->set_window(4);
    const auto third = call(revision).variable().definition();
    EXPECT_EQ(third.name(), original.name());
    EXPECT_NE(third.digest(), original.digest());
    auto duplicate = submit;
    auto& changed_factor = *duplicate.mutable_submit()->mutable_factor_request();
    *changed_factor.mutable_variable_definition() = protocol::variable_object(third);
    changed_factor.mutable_lookbacks()->Clear();
    changed_factor.add_lookbacks(4);
    EXPECT_THROW(call(duplicate), std::runtime_error);
    auto wrong_definition = submit;
    wrong_definition.mutable_submit()->set_id("expression-wrong");
    wrong_definition.mutable_submit()
        ->mutable_factor_request()
        ->mutable_variable_definition()
        ->set_version(std::string(64, 'f'));
    EXPECT_THROW(call(wrong_definition), std::runtime_error);
    EXPECT_EQ(call(result).factor().SerializeAsString(), evidence.factor().SerializeAsString());
    for (const bool missing : {false, true}) {
        const std::string id =
            missing ? "expression-prepared-missing" : "expression-prepared-present";
        service.reset();
        {
            tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
            auto prepared = task_submission(store, id, spec);
            asterion::tasks::set_program_artifact(prepared, fixture_program_artifact);
            store.admit_submission(prepared);
            prepared.prepare_files();
            const auto captured = store.prepared_submission_record(id);
            EXPECT_EQ(
                captured.execution().variable_definition().SerializeAsString(),
                original.SerializeAsString());
            EXPECT_EQ(
                captured.record().task().variable_definition().SerializeAsString(),
                protocol::variable_object(original).SerializeAsString());
            if (missing)
                std::filesystem::remove(directory.path / id / "input.pb");
        }
        start();
        research::proto::ResearchRequest recover;
        recover.mutable_recover_submission()->set_id(id);
        EXPECT_EQ(
            call(recover, {}, 10s).task().variable_definition().SerializeAsString(),
            protocol::variable_object(original).SerializeAsString());
        const auto recovered = testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            id);
        ASSERT_EQ(recovered.state(), task_wire::SUCCEEDED) << recovered.error();
        research::proto::ResearchRequest read;
        read.mutable_result()->set_id(id);
        const auto recovered_result = call(read);
        EXPECT_EQ(
            recovered_result.result_task().factor().variable_definition().SerializeAsString(),
            original.SerializeAsString());
        EXPECT_EQ(
            recovered_result.factor().SerializeAsString(),
            evidence.factor().SerializeAsString());
    }
    data_service.reset();
    service.reset();
    start();
    const auto replay = call(result);
    EXPECT_EQ(
        replay.result_task().factor().variable_definition().SerializeAsString(),
        original.SerializeAsString());
    EXPECT_EQ(replay.factor().SerializeAsString(), evidence.factor().SerializeAsString());
    EXPECT_EQ(replay.result_task().program_artifact(), fixture_program_artifact);
    EXPECT_EQ(call(get).variable().definition().SerializeAsString(), original.SerializeAsString());
    EXPECT_FALSE(data_service);
}

TEST_F(TaskProcess, RawSamplesUseFixedDataWithoutAnyFactorAndRestoreMissingPreparation)
{
    const auto dataset = published_dataset({100, -1, 0, 2, -2, 4}, "raw-sample-source");
    research::proto::ResearchRequest submit;
    auto& selected = *submit.mutable_submit();
    selected.set_id("raw-sample");
    selected.mutable_data_owner()->set_node_id("fixture-node");
    selected.mutable_data_owner()->set_service_id("historical-data");
    auto& request = *selected.mutable_sample();
    *request.mutable_selection() = protocol::bar_dataset_selection(dataset);
    request.set_use_scope("Synthetic raw close exploration, not historical eligibility proof");
    request.set_mode(research::proto::SampleRequest::POST_HOC);
    request.set_development_end(4);
    const auto accepted = call(submit, {}, 10s).task();
    EXPECT_EQ(accepted.kind(), task_wire::SAMPLE);
    const auto complete = testing_support::wait_task(
        [&](auto value) {
            return call(value);
        },
        "raw-sample");
    ASSERT_EQ(complete.state(), task_wire::SUCCEEDED) << complete.error();
    research::proto::ResearchRequest read;
    read.mutable_result()->set_id("raw-sample");
    const auto evidence = call(read);
    ASSERT_TRUE(evidence.has_sample());
    EXPECT_EQ(evidence.result_task().program_artifact(), fixture_program_artifact);
    ASSERT_EQ(evidence.sample().rows_size(), 6);
    EXPECT_EQ(evidence.sample().rows(3).role(), research::proto::SampleRow::DEVELOPMENT);
    EXPECT_EQ(evidence.sample().rows(4).role(), research::proto::SampleRow::HOLDOUT);
    const auto owned = evidence.result_task().sample();
    EXPECT_EQ(owned.dataset().bars(1).close().units(), -100000000LL);
    EXPECT_EQ(owned.dataset().bars(2).close().units(), 0);
    EXPECT_EQ(owned.request().mode(), research::proto::SampleRequest::POST_HOC);
    resource::proto::Object consumer;
    *consumer.mutable_owner() = owned.owner();
    consumer.set_kind("sample");
    consumer.set_id("raw-sample");
    consumer.set_version(sha256_bytes(owned.SerializeAsString()));
    protocol::DataClient data(endpoint + ".data", "historical-data");
    data::proto::DataRequest hold;
    *hold.mutable_retention()->mutable_get() = consumer;
    const auto retained = data.call(hold).retention();
    EXPECT_NE(retained.phase(), data::proto::DataRetentionRecord::RELEASED);
    ASSERT_EQ(retained.retention().versions_size(), 2);
    EXPECT_EQ(retained.retention().consumer().SerializeAsString(), consumer.SerializeAsString());
    service.reset();
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        auto preparation = task_submission(store, "raw-prepared", owned);
        asterion::tasks::set_program_artifact(preparation, fixture_program_artifact);
        store.admit_submission(preparation);
        preparation.prepare_files();
        EXPECT_LT(
            store.prepared_submission_record("raw-prepared").execution().ByteSizeLong(),
            owned.ByteSizeLong());
        std::filesystem::remove(directory.path / "raw-prepared/input.pb");
    }
    start();
    research::proto::ResearchRequest recover;
    recover.mutable_recover_submission()->set_id("raw-prepared");
    EXPECT_EQ(call(recover, {}, 10s).task().kind(), task_wire::SAMPLE);
    const auto recovered = testing_support::wait_task(
        [&](auto value) {
            return call(value);
        },
        "raw-prepared");
    ASSERT_EQ(recovered.state(), task_wire::SUCCEEDED) << recovered.error();
    research::proto::ResearchRequest recovered_read;
    recovered_read.mutable_result()->set_id("raw-prepared");
    EXPECT_EQ(
        call(recovered_read).sample().SerializeAsString(),
        evidence.sample().SerializeAsString());
    data_service.reset();
    service.reset();
    start();
    EXPECT_FALSE(data_service);
    EXPECT_EQ(call(submit).task().submission_sequence(), accepted.submission_sequence());
    auto changed = submit;
    changed.mutable_submit()->mutable_sample()->set_use_scope("different exploration");
    EXPECT_THROW(call(changed), std::runtime_error);
    EXPECT_EQ(call(read).sample().SerializeAsString(), evidence.sample().SerializeAsString());
    EXPECT_EQ(
        call(recovered_read).sample().SerializeAsString(),
        evidence.sample().SerializeAsString());
    EXPECT_EQ(call(read).result_task().sample().SerializeAsString(), owned.SerializeAsString());
    research::proto::ResearchRequest statistics;
    statistics.mutable_submit()->set_id("raw-statistics");
    auto& selection = *statistics.mutable_submit()->mutable_statistics();
    *selection.mutable_source()->mutable_owner() = owned.owner();
    selection.mutable_source()->set_kind("sample_result");
    selection.mutable_source()->set_id("raw-sample");
    selection.mutable_source()->set_version(evidence.result_task().result_digest());
    selection.set_column(research::proto::StatisticsRequest::RAW_CLOSE);
    selection.set_role(research::proto::StatisticsRequest::DEVELOPMENT);
    selection.set_use_scope("Synthetic original raw-close distribution with Data offline");
    selection.set_end_index(6);
    selection.set_bins(3);
    selection.set_grouping(research::proto::StatisticsRequest::TRADING_DAY);
    call(statistics, {}, 10s);
    const auto analysed = testing_support::wait_task(
        [&](auto value) {
            return call(value);
        },
        "raw-statistics");
    ASSERT_EQ(analysed.state(), task_wire::SUCCEEDED) << analysed.error();
    research::proto::ResearchRequest statistics_read;
    statistics_read.mutable_result()->set_id("raw-statistics");
    const auto report = call(statistics_read);
    EXPECT_EQ(report.statistics().summary().valid(), 4U);
    EXPECT_DOUBLE_EQ(report.statistics().summary().mean().value(), 25.25);
    ASSERT_EQ(report.statistics().groups_size(), 1);
    EXPECT_EQ(report.statistics().groups(0).trading_day(), owned.dataset().bars(0).trading_day());
    EXPECT_EQ(report.statistics().rows(0).trading_day(), owned.dataset().bars(0).trading_day());
    EXPECT_DOUBLE_EQ(report.statistics().groups(0).summary().mean().value(), 25.25);
    auto changed_grouping = statistics;
    changed_grouping.mutable_submit()->mutable_statistics()->set_grouping(
        research::proto::StatisticsRequest::NONE);
    EXPECT_THROW(call(changed_grouping), std::runtime_error);
    EXPECT_DOUBLE_EQ(report.statistics().summary().minimum().value(), -1);
    EXPECT_DOUBLE_EQ(report.statistics().rows(2).value(), 0);
    EXPECT_FALSE(report.statistics().rows(4).has_value());
    EXPECT_EQ(report.statistics().price_power(), 1);
    auto wrong_column = statistics;
    wrong_column.mutable_submit()->mutable_statistics()->set_column(
        research::proto::StatisticsRequest::FORWARD_RETURN);
    EXPECT_THROW(call(wrong_column), std::runtime_error);
    research::proto::ResearchRequest retention;
    retention.mutable_retirement()->set_id("raw-sample");
    EXPECT_EQ(call(retention).retirement().consumers_size(), 1);
    service.reset();
    start();
    EXPECT_FALSE(data_service);
    EXPECT_EQ(
        call(statistics_read).statistics().SerializeAsString(),
        report.statistics().SerializeAsString());
    EXPECT_EQ(call(read).sample().SerializeAsString(), evidence.sample().SerializeAsString());
}

TEST_F(TaskProcess, StatisticsConsumeOriginalFactorWithoutDataAndRecoverExactPreparedInput)
{
    auto spec = maximum_factor_input();
    std::vector<int> prices(90);
    for (int i = 0; i < 90; ++i)
        prices[static_cast<std::size_t>(i)] = 100 + i + i % 3;
    *factor_bars(spec) = published_dataset(prices, "statistics-source-data");
    spec.mutable_data_owner()->set_node_id("fixture-node");
    spec.mutable_data_owner()->set_service_id("historical-data");
    spec.set_dataset_revision(protocol::factor_revision(spec.series()));
    spec.set_factor(protocol::proto::PRICE_MOMENTUM);
    spec.set_retain_observations(true);
    spec.set_horizon(3);
    spec.set_holdout_start(45);
    spec.mutable_lookbacks()->Clear();
    spec.add_lookbacks(2);
    task_wire::Task fixed;
    fixed.set_kind(task_wire::FACTOR);
    *fixed.mutable_factor() = spec;
    research::proto::ResearchRequest source_submit;
    source_submit.mutable_submit()->set_id("statistics-source");
    *source_submit.mutable_submit()->mutable_factor_request() =
        protocol::task_execution(fixed, "").factor();
    *source_submit.mutable_submit()->mutable_data_owner() = spec.data_owner();
    call(source_submit, {}, 10s);
    const auto original = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "statistics-source");
    ASSERT_EQ(original.state(), task_wire::SUCCEEDED) << original.error();
    research::proto::ResearchRequest source_read;
    source_read.mutable_result()->set_id("statistics-source");
    const auto source = call(source_read);
    data_service.reset();
    research::proto::ResearchRequest submit;
    submit.mutable_submit()->set_id("statistics-report");
    auto& selected = *submit.mutable_submit()->mutable_statistics();
    auto& object = *selected.mutable_source();
    object.mutable_owner()->set_node_id("fixture-node");
    object.mutable_owner()->set_service_id("task");
    object.mutable_owner()->set_domain("research");
    object.set_kind("factor_result");
    object.set_id("statistics-source");
    object.set_version(source.result_task().result_digest());
    selected.set_use_scope("Synthetic original development feature distribution");
    selected.set_column(research::proto::StatisticsRequest::FEATURE);
    selected.set_role(research::proto::StatisticsRequest::DEVELOPMENT);
    selected.set_end_index(90);
    selected.set_bins(8);
    const auto admitted = call(submit, {}, 10s).task();
    const auto done = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "statistics-report");
    ASSERT_EQ(done.state(), task_wire::SUCCEEDED) << done.error();
    EXPECT_EQ(call(submit).task().submission_sequence(), admitted.submission_sequence());
    auto changed = submit;
    changed.mutable_submit()->mutable_statistics()->set_bins(4);
    EXPECT_THROW(call(changed), std::runtime_error);
    research::proto::ResearchRequest read;
    read.mutable_result()->set_id("statistics-report");
    const auto report = call(read);
    ASSERT_TRUE(report.has_statistics());
    EXPECT_EQ(report.statistics().summary().valid(), 43U);
    EXPECT_EQ(report.statistics().summary().allowed(), 45U);
    EXPECT_EQ(report.result_task().program_artifact(), fixture_program_artifact);
    EXPECT_EQ(report.statistics().source().SerializeAsString(), object.SerializeAsString());
    service.reset();
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        EXPECT_EQ(store.retirement("statistics-source", "fixture-node").consumers_size(), 1);
        const auto captured = store.get("statistics-report").statistics();
        auto prepared = task_submission(store, "statistics-prepared", captured);
        asterion::tasks::set_program_artifact(prepared, fixture_program_artifact);
        store.admit_submission(prepared);
        prepared.prepare_files();
        std::filesystem::remove(directory.path / "statistics-prepared/input.pb");
    }
    start();
    EXPECT_FALSE(data_service);
    research::proto::ResearchRequest recover;
    recover.mutable_recover_submission()->set_id("statistics-prepared");
    EXPECT_EQ(call(recover, {}, 10s).task().kind(), task_wire::STATISTICS);
    const auto recovered = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "statistics-prepared");
    ASSERT_EQ(recovered.state(), task_wire::SUCCEEDED) << recovered.error();
    research::proto::ResearchRequest recovered_read;
    recovered_read.mutable_result()->set_id("statistics-prepared");
    EXPECT_EQ(
        call(recovered_read).statistics().SerializeAsString(),
        report.statistics().SerializeAsString());
    EXPECT_EQ(call(read).statistics().SerializeAsString(), report.statistics().SerializeAsString());
    EXPECT_EQ(call(source_read).factor().SerializeAsString(), source.factor().SerializeAsString());
    research::proto::ResearchRequest retirement;
    retirement.mutable_retirement()->set_id("statistics-report");
    retirement.mutable_retirement()->set_execute(true);
    const auto retired = call(retirement).retirement();
    EXPECT_EQ(retired.state(), task_wire::TaskRetirement::RETIRED);
    EXPECT_TRUE(retired.source_release_confirmed());
    EXPECT_EQ(call(read).statistics().SerializeAsString(), report.statistics().SerializeAsString());
    retirement.mutable_retirement()->set_id("statistics-source");
    retirement.mutable_retirement()->set_execute(false);
    EXPECT_EQ(call(retirement).retirement().consumers_size(), 1);
}

TEST_F(TaskProcess, FixedExpressionHoldoutRecoversOriginalRolesAndValuesWithoutData)
{
    research::proto::ResearchRequest registration;
    auto& mutation = *registration.mutable_variable()->mutable_mutation();
    mutation.set_id("holdout-variable-register");
    auto& definition = *mutation.mutable_registration();
    definition.set_id("holdout-variable");
    definition.set_name("Synthetic causal holdout predictor");
    definition.set_purpose("Fixed definition, development and holdout labels remain separate");
    definition.set_expression("close / lag(close,window) - 1");
    definition.set_window(2);
    const auto original = call(registration).variable().definition();
    auto spec = maximum_factor_input();
    std::vector<int> prices(90);
    for (int i = 0; i < 90; ++i)
        prices[static_cast<std::size_t>(i)] = 100 + i + i % 3;
    *factor_bars(spec) = published_dataset(prices, "expression-holdout-data");
    spec.mutable_data_owner()->set_node_id("fixture-node");
    spec.mutable_data_owner()->set_service_id("historical-data");
    spec.set_dataset_revision(protocol::factor_revision(spec.series()));
    spec.set_factor(protocol::proto::FACTOR_UNSPECIFIED);
    spec.set_retain_observations(true);
    spec.set_horizon(3);
    spec.set_holdout_start(45);
    spec.mutable_lookbacks()->Clear();
    spec.add_lookbacks(2);
    *spec.mutable_variable_definition() = original;
    task_wire::Task fixed;
    fixed.set_kind(task_wire::FACTOR);
    *fixed.mutable_factor() = spec;
    research::proto::ResearchRequest submit;
    auto& admission = *submit.mutable_submit();
    admission.set_id("expression-held");
    *admission.mutable_factor_request() = protocol::task_execution(fixed, "").factor();
    *admission.mutable_data_owner() = spec.data_owner();
    const auto accepted = call(submit, {}, 10s).task();
    const auto finished = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "expression-held");
    ASSERT_EQ(finished.state(), task_wire::SUCCEEDED) << finished.error();
    EXPECT_EQ(call(submit).task().submission_sequence(), accepted.submission_sequence());
    research::proto::ResearchRequest read;
    read.mutable_result()->set_id("expression-held");
    const auto evidence = call(read);
    EXPECT_EQ(evidence.result_task().program_artifact(), fixture_program_artifact);
    EXPECT_EQ(
        evidence.result_task().factor().variable_definition().SerializeAsString(),
        original.SerializeAsString());
    EXPECT_EQ(evidence.result_task().factor().holdout_start(), 45U);
    ASSERT_EQ(evidence.factor().partitions_size(), 2);
    EXPECT_EQ(evidence.factor().partitions(0).sample_count(), 40U);
    EXPECT_EQ(evidence.factor().partitions(1).sample_count(), 42U);
    EXPECT_EQ(evidence.factor().purged_count(), 3U);
    EXPECT_TRUE(evidence.factor().observations(42).label_crosses_boundary());
    EXPECT_FALSE(evidence.factor().observations(42).included());
    EXPECT_EQ(evidence.factor().observations(45).role(), factor::proto::FactorObservation::HOLDOUT);
    EXPECT_FALSE(evidence.factor().observations(89).has_forward_return());
    service.reset();
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        auto prepared = task_submission(store, "expression-held-prepared", spec);
        asterion::tasks::set_program_artifact(prepared, fixture_program_artifact);
        store.admit_submission(prepared);
        prepared.prepare_files();
        EXPECT_EQ(
            store.prepared_submission_record("expression-held-prepared")
                .execution()
                .factor()
                .holdout_start(),
            45U);
        std::filesystem::remove(directory.path / "expression-held-prepared/input.pb");
    }
    start();
    research::proto::ResearchRequest recover;
    recover.mutable_recover_submission()->set_id("expression-held-prepared");
    EXPECT_EQ(call(recover, {}, 10s).task().variable_definition().version(), original.digest());
    const auto recovered = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "expression-held-prepared");
    ASSERT_EQ(recovered.state(), task_wire::SUCCEEDED) << recovered.error();
    research::proto::ResearchRequest recovered_read;
    recovered_read.mutable_result()->set_id("expression-held-prepared");
    EXPECT_EQ(
        call(recovered_read).factor().SerializeAsString(),
        evidence.factor().SerializeAsString());
    data_service.reset();
    service.reset();
    start();
    EXPECT_FALSE(data_service);
    EXPECT_EQ(call(read).factor().SerializeAsString(), evidence.factor().SerializeAsString());
    const auto replay = call(recovered_read);
    EXPECT_EQ(replay.factor().SerializeAsString(), evidence.factor().SerializeAsString());
    EXPECT_EQ(
        replay.result_task().factor().variable_definition().SerializeAsString(),
        original.SerializeAsString());
    EXPECT_EQ(replay.result_task().program_artifact(), fixture_program_artifact);
}

TEST_F(TaskProcess, ServiceOwnsConcurrencyCancellationAndUpgradeDrain)
{
    service.reset();
    replace_file_durably(sockets / "research.json", R"({"task_concurrency":1})");
    auto spec = maximum_factor_input();
    // A quarter of the largest input runs for seconds, far longer than it takes
    // to observe it running and cancel it; seeding the largest one took a minute.
    *factor_bars(spec) =
        published_dataset(std::vector<int>(protocol::max_dataset_bars / 4, 100), "pool-input");
    spec.set_dataset_revision(spec.series(0).bars().revision());
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        persist(store, "first", spec);
        persist(store, "second", spec);
    }
    start();
    research::proto::ResearchRequest get;
    get.mutable_metadata_get()->set_id("first");
    const auto deadline = std::chrono::steady_clock::now() + testing_support::bound(10s);
    while (call(get).task().state() == task_wire::QUEUED) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_EQ(call(get).task().state(), task_wire::RUNNING);
    get.mutable_metadata_get()->set_id("second");
    ASSERT_EQ(call(get).task().state(), task_wire::QUEUED);
    research::proto::ResearchRequest freeze;
    freeze.mutable_quiesce();
    ASSERT_TRUE(call(freeze, endpoint + ".health").has_health());
    research::proto::ResearchRequest stop;
    stop.mutable_quiesce()->set_stop(true);
    EXPECT_THROW(call(stop, endpoint + ".health"), std::runtime_error);
    research::proto::ResearchRequest cancel;
    cancel.mutable_cancel()->set_id("first");
    ASSERT_EQ(call(cancel).task().state(), task_wire::CANCEL_REQUESTED);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "first")
            .state(),
        task_wire::CANCELLED);
    EXPECT_EQ(call(get).task().state(), task_wire::QUEUED);
    ASSERT_TRUE(call(stop, endpoint + ".health").has_health());
    ASSERT_TRUE(service->wait(10s));
    ASSERT_EQ(service->exit_code(), 0);
    start();
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "second")
            .state(),
        task_wire::SUCCEEDED);
}

TEST_F(TaskProcess, MaximumFactorInputCompletesAndRestartedResultReadKeepsControlsAvailable)
{
    service.reset();
    auto spec = maximum_factor_input();
    *factor_bars(spec) =
        published_dataset(std::vector<int>(protocol::max_dataset_bars, 100), "maximum");
    spec.set_dataset_revision(spec.series(0).bars().revision());
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        persist(store, "maximum", spec);
        persist(store, "other", input());
    }
    start(testing_support::bound(10s));
    const auto completed = testing_support::wait_task(
        [&](auto request) {
            const auto begin = std::chrono::steady_clock::now();
            auto response = call(request);
            EXPECT_LT(std::chrono::steady_clock::now() - begin, testing_support::bound(1s));
            return response;
        },
        "maximum");
    ASSERT_EQ(completed.state(), task_wire::SUCCEEDED) << completed.error();
    service.reset();
    start(testing_support::bound(10s));
    // The first read after restart validates saved result evidence without running the engine.
    research::proto::ResearchRequest request;
    request.mutable_result()->set_id("maximum");
    auto pending = std::async(std::launch::async, [&] {
        return call(request, "", protocol::task_verification_timeout + 5s);
    });
    unsigned observations = 0;
    std::chrono::steady_clock::duration longest{};
    while (pending.wait_for(10ms) != std::future_status::ready) {
        research::proto::ResearchRequest list;
        list.mutable_list()->set_limit(200);
        const auto begin = std::chrono::steady_clock::now();
        EXPECT_EQ(call(list).tasks().tasks_size(), 2);
        longest = std::max(longest, std::chrono::steady_clock::now() - begin);
        ++observations;
    }
    const auto response = pending.get();
    EXPECT_GT(observations, 0U);
    EXPECT_LT(longest, testing_support::bound(1s));
    EXPECT_EQ(response.factor().samples_size(), static_cast<int>(protocol::max_dataset_bars) - 3);
    EXPECT_EQ(response.result_task().factor().SerializeAsString(), spec.SerializeAsString());
    RecordProperty(
        "factor_json_bytes",
        protocol::decode_factor_result(spec, response.factor()).dump().size());
    research::proto::ResearchRequest cancel;
    cancel.mutable_cancel()->set_id("other");
    EXPECT_TRUE(call(cancel).has_task());
    RecordProperty("concurrent_status_observations", observations);
    RecordProperty(
        "longest_status_ms",
        std::chrono::duration_cast<std::chrono::milliseconds>(longest).count());
}

TEST_F(TaskProcess, DistinctMaximumInputsExposeRecoveryHealthBeforeAdmissions)
{
    service.reset();
    constexpr int count = 8;
    auto spec = maximum_factor_input();
    std::vector<std::string> expected;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        for (int i = 0; i < count; ++i) {
            auto* dataset = factor_bars(spec);
            auto* last = dataset->mutable_bars(dataset->bars_size() - 1);
            const auto units = d("100").raw() + static_cast<std::int64_t>(i) * d("1").raw();
            for (auto* price :
                {last->mutable_open(),
                    last->mutable_high(),
                    last->mutable_low(),
                    last->mutable_close()})
                price->set_units(units);
            dataset->set_revision(protocol::bar_dataset_revision(*dataset));
            spec.set_dataset_revision(dataset->revision());
            expected.push_back(dataset->revision());
            persist(store, "unique-" + std::to_string(i), spec);
        }
    }
    const auto begin = std::chrono::steady_clock::now();
    launch();
    unsigned recovering = 0;
    std::string instance;
    const auto deadline = begin + testing_support::bound(60s);
    while (std::chrono::steady_clock::now() < deadline) {
        research::proto::ResearchRequest ping;
        ping.mutable_heartbeat();
        task_wire::TaskResponse response;
        try {
            response = call(ping, endpoint + ".health");
        } catch (const std::exception&) {
            ASSERT_FALSE(service->exited());
            std::this_thread::sleep_for(10ms);
            continue;
        }
        ASSERT_TRUE(response.has_health());
        const auto& health = response.health();
        if (instance.empty())
            instance = health.instance_id();
        EXPECT_EQ(health.instance_id(), instance);
        EXPECT_FALSE(health.recovery_required());
        if (health.initialized())
            break;
        ++recovering;
        research::proto::ResearchRequest dispatch;
        dispatch.mutable_list()->set_limit(1);
        EXPECT_THROW(call(dispatch, endpoint + ".health"), std::runtime_error);
        std::this_thread::sleep_for(20ms);
    }
    // Recovery may finish before the first poll; while it runs, health says so
    // and dispatch is refused.
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    research::proto::ResearchRequest list;
    list.mutable_list()->set_limit(200);
    const auto tasks = call(list).tasks();
    ASSERT_EQ(tasks.tasks_size(), count);
    for (int i = 0; i < count; ++i) {
        EXPECT_EQ(tasks.tasks(i).state(), task::proto::QUEUED);
        EXPECT_EQ(tasks.tasks(i).attempt(), 0U);
        EXPECT_EQ(tasks.tasks(i).source_name(), expected[static_cast<std::size_t>(i)]);
        EXPECT_EQ(tasks.tasks(i).definition_case(), task::proto::Task::DEFINITION_NOT_SET);
    }
    RecordProperty("distinct_maximum_input_count", count);
    RecordProperty("recovery_heartbeats", recovering);
    RecordProperty(
        "recovery_ms",
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin)
            .count());
}

TEST_F(TaskProcess, SilentExternalPeerDoesNotBlockQueriesAndCancellation)
{
    submit("cancel-me");
    auto silent = ipc::Channel::connect(endpoint, 2s);
    research::proto::ResearchRequest cancel;
    cancel.mutable_cancel()->set_id("cancel-me");
    EXPECT_EQ(call(cancel).task().state(), task_wire::CANCELLED);
}

TEST_F(TaskProcess, DataAdmissionPersistsExactReviewedRange)
{
    service.reset();
    const auto seeded = test::seed_history(warehouse.path, {100, 101}, "update-rpc");
    start_data();
    protocol::DataClient data(endpoint + ".data", "historical-data");
    data::proto::DataRequest preview;
    *preview.mutable_update_plan() = protocol::encode_history_update_query(
        {{"dataset_id", seeded.at("source_dataset_ids").at(0)},
            {"daily", nullptr},
            {"mode", "extend"},
            {"end_day", "2026-09-26"},
            {"requests_per_minute", 60}});
    const auto plan = data.call(preview).update_plan();
    data::proto::DataRequest capture;
    auto* authorization = capture.mutable_authorize_download();
    authorization->set_task_instance("historical-data");
    authorization->set_task_id("incremental");
    authorization->set_credential("fixture");
    *authorization->mutable_minutes() = plan.minutes();
    const auto captured = data.call(capture).download_authorization();
    data::proto::DataRequest submit;
    submit.mutable_acquisition_task()->mutable_submit()->set_id("wrong-task");
    submit.mutable_acquisition_task()->mutable_submit()->set_authorization(captured.id());
    EXPECT_THROW(data.call(submit), Error);
    data::proto::DataRequest get;
    get.mutable_acquisition_task()->mutable_get()->set_id("incremental");
    EXPECT_THROW(data.call(get), Error);
    submit.mutable_acquisition_task()->mutable_submit()->set_id("incremental");
    const auto created = data.call(submit).acquisition_task().task();
    EXPECT_EQ(created.id(), "incremental");
    EXPECT_EQ(created.kind(), task_wire::MINUTE_DOWNLOAD);
    EXPECT_EQ(
        data.call(get).acquisition_task().task().minutes().SerializeAsString(),
        plan.minutes().SerializeAsString());
    EXPECT_EQ(data.call(submit).acquisition_task().task().id(), created.id());
    data_service.reset();
    std::filesystem::remove(endpoint + ".data");
    std::filesystem::remove(endpoint + ".data-workers");
    start_data();
    EXPECT_EQ(
        data.call(get).acquisition_task().task().minutes().SerializeAsString(),
        plan.minutes().SerializeAsString());
    EXPECT_EQ(data.call(preview).update_plan().id(), plan.id());
}

TEST_F(TaskProcess, HistoryUsageKeepsTaskReferencesAfterCancellationAndRestart)
{
    service.reset();
    std::vector<int> prices(40);
    for (std::size_t i = 0; i < prices.size(); ++i)
        prices[i] = 100 + static_cast<int>(i) + static_cast<int>(i % 3);
    const auto seeded = test::seed_history(warehouse.path, prices, "usage");
    const auto request = composed_request({seeded});
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        data::Store data_store(warehouse.path, "historical-data");
        const auto data =
            data::resolve_bar_dataset(data_store.tables(), data_store.sources(request));
        auto backtest = input();
        *backtest.mutable_paper()->mutable_contracts(0)->mutable_dataset() = data;
        backtest.set_dataset_revision(protocol::dataset_revision(backtest.paper()));
        persist(store, "usage-backtest", backtest);
        store.commit(store.cancel("usage-backtest")).task();
        factor::proto::FactorInput factor;
        factor.set_factor(protocol::proto::PRICE_MOMENTUM);
        *factor_bars(factor) = data;
        factor.set_dataset_revision(data.revision());
        factor.add_lookbacks(1);
        factor.set_horizon(1);
        factor.set_full_sample(true);
        persist(store, "usage-factor", factor);
        store.commit(store.cancel("usage-factor")).task();
    }
    start();
    research::proto::ResearchRequest query;
    query.mutable_history_usage()->set_id(seeded.at("source_dataset_ids").at(0).get<std::string>());
    const auto first = call(query).history_usage();
    ASSERT_EQ(first.references_size(), 2);
    std::set<int> kinds;
    for (const auto& row : first.references()) {
        kinds.insert(row.kind());
        ASSERT_EQ(row.roles_size(), 1);
        EXPECT_EQ(row.roles(0), data::proto::HISTORY_MARKET);
    }
    EXPECT_EQ(kinds, (std::set<int>{data::proto::HISTORY_BACKTEST, data::proto::HISTORY_FACTOR}));
    EXPECT_EQ(protocol::decode_history_usage(first).at("references").size(), 2);
    service.reset();
    start();
    EXPECT_EQ(call(query).history_usage().SerializeAsString(), first.SerializeAsString());
    query.mutable_history_usage()->set_id(std::string(64, '0'));
    EXPECT_EQ(call(query).history_usage().references_size(), 0);
    query.mutable_history_usage()->set_id("../escape");
    EXPECT_THROW(call(query), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(directory.path / "history"));
    data_service.reset();
    std::filesystem::remove_all(warehouse.path); // Only this test's isolated warehouse.
    query.mutable_history_usage()->set_id(first.dataset_id());
    EXPECT_EQ(call(query).history_usage().SerializeAsString(), first.SerializeAsString());
}

TEST_F(TaskProcess, HistoryUsageFindsImportedTaskWithoutLocalArchiveCopy)
{
    service.reset();
    const auto spec = input();
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        persist(store, "external-origin", spec);
        store.commit(store.cancel("external-origin")).task();
    }
    EXPECT_FALSE(std::filesystem::exists(directory.path / "history"));
    start();
    research::proto::ResearchRequest query;
    query.mutable_history_usage()->set_id(
        spec.paper().contracts(0).dataset().source_dataset_ids(0));
    const auto result = call(query).history_usage();
    ASSERT_EQ(result.references_size(), 1);
    EXPECT_EQ(result.references(0).id(), "external-origin");
    EXPECT_EQ(result.references(0).kind(), data::proto::HISTORY_BACKTEST);
    EXPECT_EQ(result.references(0).roles(0), data::proto::HISTORY_MARKET);
    service.reset();
    start();
    EXPECT_EQ(call(query).history_usage().SerializeAsString(), result.SerializeAsString());
}

TEST(ResearchHost, AdmissionExpiresWithoutAbandoningAnAcceptedCommit)
{
    service::reset_stop_request();
    TaskDirectory directory;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        tasks::submit(store, "queued", input());
    }
    {
        tasks::ResearchHost host(
            {.directory = directory.path,
                .service = "task",
                .discovery_endpoint = "/unused-review-discovery.sock"});
        host.open();
        const auto operation = [&](research::proto::ResearchRequest request) {
            request.set_version(1);
            request.set_service_id("task");
            request.set_correlation_id(unique_process_id());
            return host.accept(request.SerializeAsString(), tasks::ResearchHost::Lane::client);
        };
        const auto poll = [&](service::RpcHost::Reply& request) {
            host.advance(service::RpcHost::Stage::running);
            return request();
        };
        const auto initial_deadline = std::chrono::steady_clock::now() + 5s;
        bool initialized = false;
        while (!initialized && std::chrono::steady_clock::now() < initial_deadline) {
            research::proto::ResearchRequest request;
            request.mutable_heartbeat();
            auto health = operation(request);
            auto answer = poll(health);
            if (answer) {
                task::proto::TaskResponse response;
                response.ParseFromString(answer->payload);
                initialized = response.health().initialized();
            }
            std::this_thread::sleep_for(5ms);
        }
        ASSERT_TRUE(initialized);

        struct Release
        {
            ~Release()
            {
                sqlite::hold_commits_for_testing(false);
            }
        } release;

        sqlite::hold_commits_for_testing(true);
        research::proto::ResearchRequest cancel_request;
        cancel_request.mutable_cancel()->set_id("queued");
        auto cancel = operation(cancel_request);
        const auto hold_deadline = std::chrono::steady_clock::now() + 3s;
        while (!sqlite::commit_waiting_for_testing()
            && std::chrono::steady_clock::now() < hold_deadline) {
            poll(cancel);
            std::this_thread::sleep_for(5ms);
        }
        const bool held = sqlite::commit_waiting_for_testing();
        research::proto::ResearchRequest list_request;
        list_request.mutable_list()->set_limit(200);
        auto list = operation(list_request);
        std::optional<service::RpcHost::Message> answer;
        const auto started = std::chrono::steady_clock::now();
        while (!answer && std::chrono::steady_clock::now() - started < 11s) {
            answer = poll(list);
            std::this_thread::sleep_for(5ms);
        }
        const bool timed_out_without_journal = answer.has_value();
        sqlite::hold_commits_for_testing(false);
        std::optional<service::RpcHost::Message> cancel_answer;
        const auto drain = std::chrono::steady_clock::now() + 5s;
        while ((!answer || !cancel_answer) && std::chrono::steady_clock::now() < drain) {
            host.advance(service::RpcHost::Stage::running);
            if (!answer)
                answer = list();
            if (!cancel_answer)
                cancel_answer = cancel();
            std::this_thread::sleep_for(5ms);
        }
        EXPECT_TRUE(held);
        EXPECT_TRUE(timed_out_without_journal);
        ASSERT_TRUE(answer.has_value());
        task::proto::TaskResponse response;
        ASSERT_TRUE(response.ParseFromString(answer->payload));
        EXPECT_EQ(response.error().code(), "unavailable");
        EXPECT_EQ(response.error().message(), "task request admission timed out");
        ASSERT_TRUE(cancel_answer);
        ASSERT_TRUE(response.ParseFromString(cancel_answer->payload));
        EXPECT_FALSE(response.has_error());
        EXPECT_EQ(response.task().state(), task::proto::CANCELLED);
    }
    tasks::Store recovered(asterion::research::task_domain(), directory.path, {"task"});
    EXPECT_EQ(recovered.describe("queued").state(), task::proto::CANCELLED);
    EXPECT_EQ(recovered.describe("queued").attempt(), 0U);
    EXPECT_EQ(recovered.list().tasks_size(), 1);
}

TEST(ResearchDataDiscovery, ResolvesStableOwnerWithoutPersistingItsLocator)
{
    node::proto::Status status;
    status.set_phase(node::proto::Status::READY);
    status.set_node_id("node-one");
    status.set_instance_id("generation-one");
    auto* data = status.add_services();
    data->set_kind(node::proto::DATA);
    data->set_id("data-one");
    data->set_state("running");
    data->set_endpoint("/first/data.sock");
    const auto first = tasks::resolve_research_data(status, {});
    EXPECT_EQ(first.owner.node_id(), "node-one");
    EXPECT_EQ(first.owner.service_id(), "data-one");
    status.set_instance_id("generation-two");
    data->set_endpoint("/second/data.sock");
    const auto second = tasks::resolve_research_data(status, first.owner);
    EXPECT_EQ(first.owner.SerializeAsString(), second.owner.SerializeAsString());
    EXPECT_NE(first.endpoint, second.endpoint);
}

TEST(ResearchDataDiscovery, AmbiguousOrUnavailableOwnerCannotSilentlyChangeSource)
{
    node::proto::Status status;
    status.set_phase(node::proto::Status::READY);
    status.set_node_id("node-one");
    for (const auto* id : {"data-one", "data-two"}) {
        auto* data = status.add_services();
        data->set_kind(node::proto::DATA);
        data->set_id(id);
        data->set_state("running");
        data->set_endpoint(std::string("/") + id + ".sock");
    }
    const auto rejects = [&](const data::proto::DataOwner& owner, ErrorCode expected) {
        try {
            (void)tasks::resolve_research_data(status, owner);
            ADD_FAILURE() << "unexpected data owner substitution";
        } catch (const Error& error) {
            EXPECT_EQ(error.code(), expected);
        }
    };
    rejects({}, ErrorCode::conflict);
    data::proto::DataOwner fixed;
    fixed.set_node_id("node-one");
    fixed.set_service_id("data-one");
    EXPECT_EQ(tasks::resolve_research_data(status, fixed).owner.service_id(), "data-one");
    status.mutable_services(0)->set_state("stopped");
    rejects(fixed, ErrorCode::unavailable);
    status.mutable_services()->DeleteSubrange(0, 1);
    rejects(fixed, ErrorCode::not_found);
    fixed.set_node_id("another-node");
    rejects(fixed, ErrorCode::conflict);
    status.set_phase(node::proto::Status::RECOVERY_REQUIRED);
    rejects({}, ErrorCode::unavailable);
}

TEST(ResearchFactsDiscovery, ExactOwnerIsResolvedWithoutADataServiceOrSubstitution)
{
    node::proto::Status status;
    status.set_phase(node::proto::Status::READY);
    status.set_node_id("node-one");
    auto* service = status.add_services();
    service->set_kind(node::proto::TRADING);
    service->set_id("trading-one");
    service->set_state("running");
    service->set_endpoint("/trading.sock");
    resource::proto::Owner owner;
    owner.set_domain("trading");
    owner.set_node_id("node-one");
    owner.set_service_id("trading-one");
    EXPECT_EQ(tasks::resolve_research_facts(status, owner).endpoint, "/trading.sock");
    service->set_state("stopped");
    EXPECT_THROW((void)tasks::resolve_research_facts(status, owner), Error);
    service->set_state("running");
    service->set_id("trading-two");
    EXPECT_THROW((void)tasks::resolve_research_facts(status, owner), Error);
    owner.set_node_id("other-node");
    EXPECT_THROW((void)tasks::resolve_research_facts(status, owner), Error);
}

TEST(ResearchFactsDiscovery, ProgramIdentityComesFromTheObservedResearchProcess)
{
    node::proto::Status status;
    status.set_phase(node::proto::Status::READY);
    status.set_node_id("node-one");
    auto* service = status.add_services();
    service->set_kind(node::proto::RESEARCH);
    service->set_id("research-one");
    service->set_state("running");
    service->set_pid(123);
    service->set_artifact(std::string(64, 'a'));
    const auto program = tasks::resolve_research_program(status, "research-one", 123);
    EXPECT_EQ(program.owner.node_id(), "node-one");
    EXPECT_EQ(program.owner.domain(), "research");
    EXPECT_EQ(program.artifact, std::string(64, 'a'));
    EXPECT_THROW((void)tasks::resolve_research_program(status, "research-one", 124), Error);
    service->set_artifact("");
    EXPECT_THROW((void)tasks::resolve_research_program(status, "research-one", 123), Error);
}

TEST(ResearchDataDiscovery, FixedInputDigestAndExecutionRetainLogicalOwner)
{
    auto spec = input();
    spec.mutable_data_owner()->set_node_id("node-one");
    spec.mutable_data_owner()->set_service_id("data-one");
    task::proto::Task task;
    *task.mutable_input() = spec;
    const auto original = sha256_bytes(spec.SerializeAsString());
    const auto execution = protocol::task_execution(task, original);
    ASSERT_TRUE(execution.has_data_owner());
    EXPECT_EQ(execution.data_owner().SerializeAsString(), spec.data_owner().SerializeAsString());
    EXPECT_EQ(execution.input_sha256(), original);
    spec.mutable_data_owner()->set_service_id("data-two");
    EXPECT_NE(sha256_bytes(spec.SerializeAsString()), original);
    EXPECT_EQ(execution.data_owner().service_id(), "data-one");
}

TEST_F(TaskProcess, DistinctTasksResolveDistinctDataOwnersAndRetainThemAcrossRestart)
{
    const auto first = process_input();
    TaskDirectory other;
    const auto selected = test::seed_history(
        other.path,
        {200, 201, 202, 201, 200, 201, 203},
        "second-owner",
        {"2024-01-01"},
        {},
        "rb",
        1,
        210,
        "test.confirmed.v1",
        "2026-10",
        100,
        "other-data");
    auto second = input();
    {
        data::Store store(other.path, "other-data");
        auto query = protocol::encode_bar_dataset_request(
            {{"source_dataset_ids", selected.at("source_dataset_ids")},
                {"daily", selected.at("daily")},
                {"begin_day", ""},
                {"end_day", ""},
                {"contract", "SHFE/rb/2026-10"}});
        *second.mutable_paper()->mutable_contracts(0)->mutable_dataset() =
            data::resolve_bar_dataset(store.tables(), store.sources(query));
        second.set_dataset_revision(protocol::dataset_revision(second.paper()));
    }
    auto other_process = std::make_unique<ChildProcess>(
        ASTERION_DATA_SERVICE_PATH,
        std::vector<std::string>{
            "--directory",
            other.path.string(),
            "--session",
            "other-data",
            "--node-id",
            "fixture-node",
            "--endpoint",
            endpoint + ".other-data",
            "--internal-endpoint",
            endpoint + ".other-internal"
        });
    discovery.reset();
    discovery = std::make_unique<testing_support::DataDiscovery>(
        endpoint + ".discovery",
        "historical-data",
        endpoint + ".data",
        std::vector<std::pair<std::string, std::string>>{{"other-data", endpoint + ".other-data"}});
    discovery->research("task", service->id(), fixture_program_artifact);
    protocol::DataClient other_client(endpoint + ".other-data", "other-data");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        try {
            data::proto::DataRequest heartbeat;
            heartbeat.mutable_heartbeat();
            if (other_client.call(heartbeat, 1s).health().initialized())
                break;
        } catch (const Error&) {
            ASSERT_FALSE(other_process->exited());
        }
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(20ms);
    }
    const auto submission = [&](const std::string& id,
                                const backtest::proto::BacktestInput& spec,
                                const std::string& owner) {
        research::proto::ResearchRequest request;
        auto* submit = request.mutable_submit();
        submit->set_id(id);
        submit->mutable_data_owner()->set_node_id("fixture-node");
        submit->mutable_data_owner()->set_service_id(owner);
        task::proto::Task fixed;
        *fixed.mutable_input() = spec;
        *submit->mutable_backtest() = protocol::task_execution(fixed, "").backtest();
        return request;
    };
    auto ambiguous = submission("ambiguous", first, "");
    EXPECT_THROW(call(ambiguous), std::runtime_error);
    EXPECT_EQ(
        call(submission("first-owner", first, "historical-data")).task().state(),
        task_wire::QUEUED);
    EXPECT_EQ(
        call(submission("second-owner", second, "other-data")).task().state(),
        task_wire::QUEUED);
    for (const auto* id : {"first-owner", "second-owner"}) {
        EXPECT_EQ(
            testing_support::wait_task(
                [&](auto request) {
                    return call(request);
                },
                id)
                .state(),
            task_wire::SUCCEEDED);
    }
    const auto saved = [&](const std::string& id) {
        research::proto::ResearchRequest request;
        request.mutable_get()->set_id(id);
        return call(request).task().input();
    };
    const auto one = saved("first-owner"), two = saved("second-owner");
    EXPECT_EQ(one.data_owner().service_id(), "historical-data");
    EXPECT_EQ(two.data_owner().service_id(), "other-data");
    EXPECT_NE(one.dataset_revision(), two.dataset_revision());
    const auto registered = [&](protocol::DataClient& data,
                                const std::string& id,
                                const backtest::proto::BacktestInput& fixed) {
        data::proto::DataRequest request;
        auto* consumer = request.mutable_retention()->mutable_get();
        consumer->mutable_owner()->set_domain("research");
        consumer->mutable_owner()->set_node_id("fixture-node");
        consumer->mutable_owner()->set_service_id("task");
        consumer->set_kind("backtest");
        consumer->set_id(id);
        consumer->set_version(sha256_bytes(fixed.SerializeAsString()));
        return data.call(request).retention();
    };
    protocol::DataClient first_client(endpoint + ".data", "historical-data");
    const auto first_hold = registered(first_client, "first-owner", one);
    const auto second_hold = registered(other_client, "second-owner", two);
    EXPECT_EQ(first_hold.phase(), data::proto::DataRetentionRecord::CONFIRMED);
    EXPECT_EQ(second_hold.phase(), data::proto::DataRetentionRecord::CONFIRMED);
    EXPECT_EQ(first_hold.retention().source().service_id(), "historical-data");
    EXPECT_EQ(second_hold.retention().source().service_id(), "other-data");
    EXPECT_TRUE(std::filesystem::is_regular_file(directory.path / "first-owner/data-retention.pb"));
    research::proto::ResearchRequest original;
    original.mutable_get()->set_id("first-owner");
    const auto accepted = call(original).task();
    research::proto::ResearchRequest reuse;
    auto* submit = reuse.mutable_submit();
    submit->set_id("wrong-fixed-owner");
    submit->mutable_data_owner()->set_service_id("other-data");
    auto* fixed = submit->mutable_backtest_reuse();
    fixed->set_choice(backtest::proto::BacktestReuse::FIXED);
    fixed->set_source_id("first-owner");
    fixed->set_source_result_digest(accepted.result_digest());
    fixed->set_source_input_sha256(sha256_bytes(one.SerializeAsString()));
    EXPECT_THROW(call(reuse), std::runtime_error);
    reuse.mutable_submit()->set_id("fixed-owner");
    reuse.mutable_submit()->mutable_data_owner()->Clear();
    EXPECT_EQ(call(reuse).task().state(), task_wire::QUEUED);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "fixed-owner")
            .state(),
        task_wire::SUCCEEDED);
    EXPECT_EQ(
        saved("fixed-owner").data_owner().SerializeAsString(),
        one.data_owner().SerializeAsString());
    service.reset();
    data_service.reset();
    other_process.reset();
    start();
    EXPECT_EQ(saved("first-owner").SerializeAsString(), one.SerializeAsString());
    EXPECT_EQ(saved("second-owner").SerializeAsString(), two.SerializeAsString());
}

TEST_F(TaskProcess, PreparedReferenceRecoversWithAndWithoutPublishedInputAndReplaysIdempotently)
{
    auto spec = process_input();
    spec.mutable_data_owner()->set_node_id("fixture-node");
    spec.mutable_data_owner()->set_service_id("historical-data");
    for (const bool missing_input : {false, true}) {
        SCOPED_TRACE(missing_input);
        const std::string id = missing_input ? "prepared-missing-input" : "prepared-input";
        service.reset();
        data::proto::DataRetention retention;
        {
            tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
            auto pending = task_submission(store, id, spec);
            preserve_program();
            asterion::tasks::set_program_artifact(pending, preserved_program);
            store.admit_submission(pending);
            pending.prepare_files();
            tasks::ResearchDataRoute route{spec.data_owner(), endpoint + ".data", "fixture-node"};
            retention = tasks::research_retention(pending.execution(), route, "task", id);
            tasks::save_research_retention(directory.path / id, retention);
            protocol::DataClient data(endpoint + ".data", "historical-data");
            data::proto::DataRequest prepare;
            *prepare.mutable_retention()->mutable_prepare() = retention;
            EXPECT_EQ(
                data.call(prepare).retention().phase(),
                data::proto::DataRetentionRecord::PREPARED);
            // Discard the reply and stop before the Research task index commit.
        }
        const auto payload = directory.path / id / "input.pb";
        const auto before = sha256_file(payload);
        if (missing_input)
            std::filesystem::remove(payload); // Isolated crash-before-payload-publication state.
        start();
        research::proto::ResearchRequest recover;
        recover.mutable_recover_submission()->set_id(id);
        EXPECT_EQ(call(recover, "", 10s).task().state(), task_wire::QUEUED);
        EXPECT_EQ(
            testing_support::wait_task(
                [&](auto request) {
                    return call(request);
                },
                id)
                .state(),
            task_wire::SUCCEEDED);
        EXPECT_EQ(call(recover).task().state(), task_wire::SUCCEEDED);
        EXPECT_EQ(sha256_file(payload), before);
        research::proto::ResearchRequest get;
        get.mutable_get()->set_id(id);
        EXPECT_EQ(call(get).task().input().SerializeAsString(), spec.SerializeAsString());
        protocol::DataClient data(endpoint + ".data", "historical-data");
        data::proto::DataRequest query;
        *query.mutable_retention()->mutable_get() = retention.consumer();
        const auto confirmed = data.call(query).retention();
        EXPECT_EQ(confirmed.phase(), data::proto::DataRetentionRecord::CONFIRMED);
        EXPECT_EQ(confirmed.retention().SerializeAsString(), retention.SerializeAsString());
        research::proto::ResearchRequest list;
        list.mutable_list()->set_limit(200);
        EXPECT_EQ(call(list).tasks().capacity().uncommitted(), 0U);
        if (missing_input) {
            data_service.reset();
            EXPECT_EQ(call(recover).task().state(), task_wire::SUCCEEDED);
            EXPECT_EQ(call(get).task().input().SerializeAsString(), spec.SerializeAsString());
        }
    }
}

TEST_F(TaskProcess, DataAcquisitionRetirementUsesOwnerProtocolAndSurvivesRestart)
{
    service.reset();
    data_service.reset();
    {
        data::Store initialize(warehouse.path, "historical-data");
    }
    std::filesystem::create_directories(warehouse.path / "acquisition-tasks");
    {
        tasks::Store store(
            asterion::data::task_domain(),
            warehouse.path / "acquisition-tasks",
            {"historical-data"});
        tasks::submit(store, refresh_authorization("rpc-retire"));
        store.commit(store.cancel("rpc-retire"));
    }
    start_data();
    protocol::DataClient client(endpoint + ".data", "historical-data");
    data::proto::DataRequest request;
    request.mutable_acquisition_task()->mutable_retirement()->set_id("rpc-retire");
    const auto preview = client.call(request, 5s).acquisition_task().retirement();
    ASSERT_EQ(preview.state(), task::proto::TaskRetirement::AVAILABLE);
    EXPECT_EQ(preview.object().owner().node_id(), "fixture-node");
    EXPECT_EQ(preview.object().owner().service_id(), "historical-data");
    EXPECT_EQ(preview.object().owner().domain(), "data");
    request.mutable_acquisition_task()->mutable_retirement()->set_execute(true);
    const auto retired = client.call(request, 5s).acquisition_task().retirement();
    ASSERT_EQ(retired.state(), task::proto::TaskRetirement::RETIRED);
    EXPECT_EQ(
        client.call(request, 5s).acquisition_task().retirement().SerializeAsString(),
        retired.SerializeAsString());
    data_service.reset();
    std::filesystem::remove(endpoint + ".data");
    std::filesystem::remove(endpoint + ".data-workers");
    start_data();
    EXPECT_EQ(
        client.call(request, 5s).acquisition_task().retirement().SerializeAsString(),
        retired.SerializeAsString());
    request.mutable_acquisition_task()->mutable_retry()->set_id("rpc-retire");
    EXPECT_THROW(client.call(request, 5s), Error);
    request.mutable_acquisition_task()->mutable_get()->set_id("rpc-retire");
    const auto evidence = client.call(request, 5s).acquisition_task().task();
    EXPECT_TRUE(evidence.has_retirement());
    EXPECT_EQ(evidence.download_authorization(), std::string(64, 'a'));
    EXPECT_EQ(evidence.state(), task::proto::CANCELLED);
}

TEST_F(TaskProcess, TableRetirementRejectsChangedReviewedObjectScope)
{
    service.reset();
    data_service.reset();
    {
        data::Store initialize(warehouse.path, "historical-data");
    }
    std::string first, middle, head, old_object;
    {
        history_files::TableStore tables(warehouse.path / "tables");
        const auto& daily = history_table("futures.daily");
        const history_files::TableSeries series{"futures.daily", 1, "fixture.daily.v1"};
        const auto partition = [&](const std::string& day, const std::string& close) {
            TableSourceRow row(daily.columns.size());
            row[0] = "SHFE/rb/2026-10";
            row[4] = close;
            return table_partition(daily, "SHFE", day, {row});
        };
        const auto a = partition("2025-01-01", "100"), b = partition("2025-01-02", "200"),
                   c = partition("2025-01-01", "300");
        const auto initial =
            tables.record(a, series, tables.write(a), std::string(64, 'a'), 1000).revision;
        first = initial.snapshot;
        old_object = initial.object;
        middle =
            tables.record(b, series, tables.write(b), std::string(64, 'a'), 2000).revision.snapshot;
        head =
            tables.record(c, series, tables.write(c), std::string(64, 'a'), 3000).revision.snapshot;
    }
    start_data();
    protocol::DataClient client(endpoint + ".data", "historical-data");
    const auto preview = [&](const std::string& id) {
        data::proto::DataRequest request;
        request.mutable_table_retirement()->set_id(id);
        return client.call(request, 10s).table_retirement();
    };
    const auto execute = [&](const data::proto::TableRetirement& reviewed) {
        data::proto::DataRequest request;
        auto* operation = request.mutable_table_retirement();
        operation->set_id(reviewed.id());
        operation->set_execute(true);
        operation->set_preview_digest(reviewed.preview_digest());
        return client.call(request, 10s).table_retirement();
    };
    const auto old = preview(middle);
    ASSERT_EQ(old.state(), data::proto::TableRetirement::AVAILABLE);
    EXPECT_TRUE(old.objects().empty());
    ASSERT_EQ(execute(preview(first)).state(), data::proto::TableRetirement::REMOVED);
    const auto current = preview(middle);
    ASSERT_EQ(current.objects_size(), 1);
    EXPECT_EQ(current.objects(0), old_object);
    EXPECT_NE(current.preview_digest(), old.preview_digest());
    EXPECT_THROW(execute(old), Error);
    EXPECT_EQ(preview(middle).state(), data::proto::TableRetirement::AVAILABLE);
    const auto path =
        warehouse.path / "tables/objects" / old_object.substr(0, 2) / (old_object + ".parquet");
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(execute(current).state(), data::proto::TableRetirement::REMOVED);
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(execute(current).state(), data::proto::TableRetirement::REMOVED);
    EXPECT_EQ(preview(head).state(), data::proto::TableRetirement::BLOCKED);
}

TEST(ResearchResourceCatalogue, FixedInputAndResultIdentitySurviveRetirementAndReadOnlyPaging)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, {"research-one"});
    tasks::submit(store, "alpha", input());
    const auto token = store.commit(store.claim("alpha")).token();
    store.commit(tasks::finish(store, "alpha", token, backtest::run(input())));
    tasks::submit(store, "beta", input());
    resource::proto::CatalogueQuery query;
    query.set_kind("backtest");
    query.set_limit(1);
    const auto first = asterion::research::task_resources(store, "fixture-node", query);
    ASSERT_EQ(first.items_size(), 1);
    EXPECT_EQ(first.items(0).object().id(), "alpha");
    EXPECT_EQ(first.next_after(), "alpha");
    EXPECT_EQ(
        first.items(0).logical_bytes(),
        std::filesystem::file_size(directory.path / "alpha/input.pb"));
    query.set_after(first.next_after());
    const auto second = asterion::research::task_resources(store, "fixture-node", query);
    ASSERT_EQ(second.items_size(), 1);
    EXPECT_EQ(second.items(0).object().id(), "beta");
    EXPECT_TRUE(second.next_after().empty());
    query.set_after("");
    query.set_kind("backtest_result");
    const auto completed = asterion::research::task_resources(store, "fixture-node", query);
    ASSERT_EQ(completed.items_size(), 1);
    EXPECT_EQ(completed.items(0).object().version(), store.describe("alpha").result_digest());
    EXPECT_FALSE(completed.items(0).has_logical_bytes());
    const auto hash = sha256_file(directory.path / "alpha/results/1.pb");
    store.commit(store.retire(store.retirement("alpha", "fixture-node").object()));
    const auto retired = asterion::research::task_resources(store, "fixture-node", query);
    ASSERT_EQ(retired.items_size(), 1);
    EXPECT_EQ(retired.items(0).state(), resource::proto::CatalogueEntry::RETIRED);
    EXPECT_EQ(
        retired.items(0).object().SerializeAsString(),
        completed.items(0).object().SerializeAsString());
    EXPECT_EQ(sha256_file(directory.path / "alpha/results/1.pb"), hash);
    EXPECT_NO_THROW(protocol::decode_resource_catalogue(retired, query, retired.owner()));
    query.set_kind("model");
    EXPECT_THROW(asterion::research::task_resources(store, "fixture-node", query), Error);
}

TEST_F(TaskProcess, ResourceCataloguesStayWithTheirOwnersWhenDataGoesOffline)
{
    service.reset();
    data_service.reset();
    const auto seeded = test::seed_history(warehouse.path, {100, 101}, "catalogue-rpc");
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        persist(store, "catalogued-input", input());
        store.commit(store.cancel("catalogued-input"));
    }
    start_data();
    start();
    research::proto::ResearchRequest research;
    research.mutable_resources()->set_kind("backtest");
    research.mutable_resources()->set_limit(1);
    const auto original = call(research).resources();
    ASSERT_EQ(original.items_size(), 1);
    EXPECT_EQ(original.items(0).object().id(), "catalogued-input");
    EXPECT_EQ(original.owner().node_id(), "fixture-node");
    EXPECT_EQ(original.owner().domain(), "research");
    protocol::DataClient client(endpoint + ".data", "historical-data");
    data::proto::DataRequest data;
    data.mutable_resources()->set_kind("minute_version");
    const auto archived = client.call(data, 5s).resources();
    ASSERT_EQ(archived.items_size(), 1);
    EXPECT_EQ(
        archived.items(0).object().id(),
        seeded.at("source_dataset_ids")[0].get<std::string>());
    EXPECT_EQ(archived.owner().node_id(), "fixture-node");
    EXPECT_EQ(archived.owner().domain(), "data");
    data_service.reset();
    std::filesystem::remove(endpoint + ".data");
    std::filesystem::remove(endpoint + ".data-workers");
    EXPECT_EQ(call(research).resources().SerializeAsString(), original.SerializeAsString());
    research.mutable_resources()->set_kind("table_snapshot");
    EXPECT_THROW(call(research), std::runtime_error);
}

TEST_F(
    TaskProcess,
    FixedWeightPortfolioUsesRealDataAdmissionAndKeepsOriginalTargetsAcrossRestartAndPreparationRecovery)
{
    (void)test::seed_history(warehouse.path, {100, 101}, "fixed-weight-catalogue");
    const std::vector<std::string>
        days{"2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24", "2026-09-25", "2026-09-28"};
    std::vector<test::DailyRow> rows;
    for (const auto& day : days)
        rows.push_back({day, 1000, 100, test::DailyTrade{1000, 1001, 999, 1000, 1000}});
    const std::array<Json, 2> tables{
        test::seed_daily_table(warehouse.path, "SHFE/rb/2026-10", rows),
        test::seed_daily_table(warehouse.path, "SHFE/hc/2026-10", rows)
    };
    start_data();
    const auto prototype = fixed_weight_input();
    research::proto::ResearchRequest submit;
    submit.mutable_submit()->set_id("fixed-weight-portfolio");
    auto& request = *submit.mutable_submit()->mutable_backtest();
    *request.mutable_deposit() = prototype.paper().deposit();
    *request.mutable_risk() = prototype.paper().risk();
    *request.add_strategies() = prototype.strategies(0);
    for (int c = 0; c < 2; ++c) {
        auto* contract = request.add_contracts();
        *contract->mutable_data()->mutable_daily() = protocol::encode_daily_table(tables[c]);
        contract->mutable_data()->set_contract(c ? "SHFE/hc/2026-10" : "SHFE/rb/2026-10");
        contract->mutable_data()->set_begin_day(days.front());
        contract->mutable_data()->set_end_day(days.back());
        *contract->mutable_cost_schedule() = prototype.paper().contracts(c).cost_schedule();
    }
    const auto admitted = call(submit, {}, 10s).task();
    const auto completed = [&](const char* id) {
        const auto status = testing_support::wait_task(
            [&](auto query) {
                return call(query);
            },
            id,
            20s);
        if (status.state() != task_wire::SUCCEEDED)
            throw std::runtime_error(status.error());
        research::proto::ResearchRequest read;
        read.mutable_result()->set_id(id);
        return call(read, {}, 10s);
    };
    const auto result = completed("fixed-weight-portfolio");
    ASSERT_EQ(result.backtest().portfolio_targets_size(), 3);
    ASSERT_EQ(result.backtest().portfolio_fills_size(), result.backtest().account().fills_size());
    EXPECT_EQ(result.backtest().portfolio_targets(0).members(0).quantity().units(), d("4").raw());
    EXPECT_EQ(result.backtest().portfolio_targets(0).members(1).quantity().units(), d("-2").raw());
    EXPECT_EQ(result.backtest().account().fees().units(), d("20").raw());
    EXPECT_EQ(result.result_task().program_artifact(), fixture_program_artifact);
    EXPECT_FALSE(result.result_task().risk_artifact().empty());
    EXPECT_NO_THROW(
        protocol::validate_backtest_result(result.result_task().input(), result.backtest()));
    const auto replay = call(submit, {}, 10s).task();
    EXPECT_EQ(replay.id(), admitted.id());
    EXPECT_EQ(replay.attempt(), 1U);
    EXPECT_EQ(replay.result_digest(), result.result_task().result_digest());
    auto changed = submit;
    changed.mutable_submit()
        ->mutable_backtest()
        ->mutable_strategies(0)
        ->mutable_fixed_weights()
        ->mutable_gross_weight_limit()
        ->set_units(d("0.9").raw());
    EXPECT_THROW(call(changed, {}, 10s), std::runtime_error);
    service.reset();
    data_service.reset();
    start();
    const auto restored = completed("fixed-weight-portfolio");
    EXPECT_EQ(restored.backtest().SerializeAsString(), result.backtest().SerializeAsString());
    EXPECT_EQ(restored.result_task().SerializeAsString(), result.result_task().SerializeAsString());
    service.reset();
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        auto preparation =
            task_submission(store, "fixed-weight-prepared", result.result_task().input());
        asterion::tasks::set_program_artifact(preparation, fixture_program_artifact);
        store.admit_submission(preparation);
        preparation.prepare_files();
        EXPECT_TRUE(store.prepared_submission_record("fixed-weight-prepared")
                .execution()
                .backtest()
                .strategies(0)
                .has_fixed_weights());
        EXPECT_TRUE(store.abandon_submission(preparation));
    }
    start_data();
    start();
    research::proto::ResearchRequest recover;
    recover.mutable_recover_submission()->set_id("fixed-weight-prepared");
    EXPECT_EQ(call(recover, {}, 10s).task().kind(), task_wire::BACKTEST);
    const auto recovered = completed("fixed-weight-prepared");
    EXPECT_EQ(
        recovered.result_task().input().SerializeAsString(),
        result.result_task().input().SerializeAsString());
    EXPECT_EQ(recovered.backtest().SerializeAsString(), result.backtest().SerializeAsString());
    research::proto::ResearchRequest list;
    list.mutable_list()->set_limit(10);
    EXPECT_EQ(call(list).tasks().tasks_size(), 2);
    research::proto::ResearchRequest freeze;
    auto& mutation = *freeze.mutable_strategy()->mutable_mutation();
    mutation.set_id("freeze-portfolio-candidate");
    auto& fixed = *mutation.mutable_freeze();
    fixed.set_id("portfolio-candidate");
    fixed.set_name("Synthetic fixed portfolio");
    fixed.set_purpose("Explicit original-condition portfolio research");
    fixed.set_limitations(
        "Retrospective synthetic reference, no independent validation or live permission");
    auto& source = *fixed.mutable_source();
    source.mutable_owner()->set_node_id("fixture-node");
    source.mutable_owner()->set_service_id("task");
    source.mutable_owner()->set_domain("research");
    source.set_kind("backtest_result");
    source.set_id("fixed-weight-portfolio");
    source.set_version(result.result_task().result_digest());
    const auto candidate = call(freeze).strategy().package();
    ASSERT_TRUE(candidate.definition().has_portfolio());
    ASSERT_EQ(candidate.definition().portfolio().contracts_size(), 2);
    EXPECT_FALSE(candidate.definition().has_contract());
    EXPECT_EQ(
        candidate.definition().portfolio().strategy().SerializeAsString(),
        prototype.strategies(0).SerializeAsString());
    EXPECT_EQ(call(freeze).strategy().package().SerializeAsString(), candidate.SerializeAsString());
    EXPECT_EQ(call(list).tasks().tasks_size(), 2);
    research::proto::ResearchRequest deliver;
    auto& creation = *deliver.mutable_strategy()->mutable_mutation();
    creation.set_id("deliver-portfolio");
    creation.mutable_deliver()->set_id("portfolio-delivery");
    creation.mutable_deliver()->set_target_task_id("portfolio-explicit-use");
    *creation.mutable_deliver()->mutable_package() = protocol::strategy_package_object(candidate);
    const auto draft = call(deliver).strategy().delivery();
    research::proto::ResearchRequest delivery_status;
    delivery_status.mutable_strategy()->mutable_delivery()->set_id(draft.id());
    EXPECT_FALSE(call(delivery_status).strategy().target_accepted());
    EXPECT_EQ(call(list).tasks().tasks_size(), 2);
    research::proto::ResearchRequest accept;
    auto& use = *accept.mutable_submit();
    use.set_id(draft.target_task_id());
    *use.mutable_strategy_use()->mutable_package() = draft.package();
    *use.mutable_strategy_use()->mutable_delivery() = protocol::strategy_delivery_object(draft);
    auto& reuse = *use.mutable_backtest_reuse();
    reuse.set_choice(backtest::proto::BacktestReuse::FIXED);
    reuse.set_source_id(draft.reference_result().id());
    reuse.set_source_result_digest(draft.reference_result().version());
    reuse.set_source_input_sha256(draft.source_input_sha256());
    auto wrong = accept;
    wrong.mutable_submit()->mutable_backtest_reuse()->mutable_fee_multiplier()->set_units(
        d("2").raw());
    EXPECT_THROW(call(wrong), std::runtime_error);
    EXPECT_NO_THROW(call(accept));
    const auto used = completed(draft.target_task_id().c_str());
    EXPECT_EQ(
        used.result_task().strategy_use().SerializeAsString(),
        use.strategy_use().SerializeAsString());
    EXPECT_EQ(
        used.result_task().input().SerializeAsString(),
        result.result_task().input().SerializeAsString());
    EXPECT_EQ(used.backtest().SerializeAsString(), result.backtest().SerializeAsString());
    EXPECT_EQ(call(accept).task().attempt(), 1U);
    EXPECT_TRUE(call(delivery_status).strategy().target_accepted());
    EXPECT_EQ(call(list).tasks().tasks_size(), 3);
    auto explicit_delivery = deliver;
    explicit_delivery.mutable_strategy()->mutable_mutation()->set_id(
        "deliver-portfolio-new-conditions");
    auto& new_delivery =
        *explicit_delivery.mutable_strategy()->mutable_mutation()->mutable_deliver();
    new_delivery.set_id("portfolio-new-conditions-delivery");
    new_delivery.set_target_task_id("portfolio-new-conditions");
    const auto explicit_draft = call(explicit_delivery).strategy().delivery();
    research::proto::ResearchRequest explicit_status;
    explicit_status.mutable_strategy()->mutable_delivery()->set_id(explicit_draft.id());
    EXPECT_FALSE(call(explicit_status).strategy().target_accepted());
    // The isolated Data writer is stopped before seeding a new immutable snapshot.
    data_service.reset();
    std::vector<test::DailyRow> revised_rows;
    for (const auto& day : days)
        revised_rows.push_back({day, 1000, 101, test::DailyTrade{1000, 1001, 999, 1000, 1000}});
    const std::array<Json, 2> revised_tables{
        test::seed_daily_table(warehouse.path, "SHFE/rb/2026-10", revised_rows),
        test::seed_daily_table(warehouse.path, "SHFE/hc/2026-10", revised_rows)
    };
    start_data();
    auto explicit_submit = submit;
    auto& configured = *explicit_submit.mutable_submit();
    configured.set_id(explicit_draft.target_task_id());
    *configured.mutable_strategy_use()->mutable_package() = explicit_draft.package();
    *configured.mutable_strategy_use()->mutable_delivery() =
        protocol::strategy_delivery_object(explicit_draft);
    auto& conditions = *configured.mutable_backtest();
    for (int c = 0; c < conditions.contracts_size(); ++c) {
        auto* member = conditions.mutable_contracts(c);
        *member->mutable_data()->mutable_daily() = protocol::encode_daily_table(revised_tables[c]);
        for (auto& version : *member->mutable_cost_schedule()->mutable_versions())
            for (auto* fee :
                {version.mutable_values()->mutable_open_fee(),
                    version.mutable_values()->mutable_close_today_fee(),
                    version.mutable_values()->mutable_close_yesterday_fee()})
                fee->set_units(fee->units() * 2);
    }
    auto wrong_policy = explicit_submit;
    wrong_policy.mutable_submit()
        ->mutable_backtest()
        ->mutable_strategies(0)
        ->mutable_fixed_weights()
        ->mutable_gross_weight_limit()
        ->set_units(d("0.9").raw());
    EXPECT_THROW(call(wrong_policy), std::runtime_error);
    EXPECT_EQ(call(list).tasks().tasks_size(), 3);
    EXPECT_NO_THROW(call(explicit_submit));
    const auto explicit_used = completed(explicit_draft.target_task_id().c_str());
    EXPECT_EQ(explicit_used.backtest().account().fees().units(), d("40").raw());
    EXPECT_EQ(
        explicit_used.result_task().strategy_use().request_sha256(),
        sha256_bytes(conditions.SerializeAsString()));
    EXPECT_NE(
        explicit_used.result_task().input().SerializeAsString(),
        result.result_task().input().SerializeAsString());
    for (int c = 0; c < 2; ++c) {
        const auto& chosen =
            explicit_used.result_task().input().paper().contracts(c).dataset().daily();
        EXPECT_EQ(chosen.snapshot(), revised_tables[c].at("snapshot").get<std::string>());
        EXPECT_NE(
            chosen.snapshot(),
            result.result_task().input().paper().contracts(c).dataset().daily().snapshot());
    }
    EXPECT_TRUE(call(explicit_status).strategy().target_accepted());
    EXPECT_EQ(call(explicit_submit).task().attempt(), 1U);
    auto changed_conditions = explicit_submit;
    changed_conditions.mutable_submit()->mutable_backtest()->mutable_deposit()->set_units(
        d("200000").raw());
    EXPECT_THROW(call(changed_conditions), std::runtime_error);
    EXPECT_EQ(
        completed("fixed-weight-portfolio").backtest().account().fees().units(),
        d("20").raw());
    EXPECT_EQ(call(freeze).strategy().package().SerializeAsString(), candidate.SerializeAsString());
    EXPECT_EQ(call(list).tasks().tasks_size(), 4);
    service.reset();
    data_service.reset();
    start();
    const auto offline_use = completed(draft.target_task_id().c_str());
    EXPECT_EQ(
        offline_use.result_task().SerializeAsString(),
        used.result_task().SerializeAsString());
    EXPECT_EQ(offline_use.backtest().SerializeAsString(), used.backtest().SerializeAsString());
    EXPECT_EQ(offline_use.service_id(), used.service_id());
    EXPECT_EQ(call(freeze).strategy().package().SerializeAsString(), candidate.SerializeAsString());
    EXPECT_TRUE(call(delivery_status).strategy().target_accepted());
    EXPECT_EQ(call(accept).task().attempt(), 1U);
    const auto offline_explicit = completed(explicit_draft.target_task_id().c_str());
    EXPECT_EQ(
        offline_explicit.result_task().SerializeAsString(),
        explicit_used.result_task().SerializeAsString());
    EXPECT_EQ(
        offline_explicit.backtest().SerializeAsString(),
        explicit_used.backtest().SerializeAsString());
    EXPECT_TRUE(call(explicit_status).strategy().target_accepted());
    EXPECT_EQ(call(explicit_submit).task().attempt(), 1U);
    auto pending_command = deliver;
    pending_command.mutable_strategy()->mutable_mutation()->set_id(
        "deliver-portfolio-pending-recovery");
    auto& pending_request =
        *pending_command.mutable_strategy()->mutable_mutation()->mutable_deliver();
    pending_request.set_id("portfolio-pending-delivery");
    pending_request.set_target_task_id("portfolio-pending-unadmitted");
    const auto pending_draft = call(pending_command).strategy().delivery();
    research::proto::ResearchRequest pending_status;
    pending_status.mutable_strategy()->mutable_delivery()->set_id(pending_draft.id());
    EXPECT_FALSE(call(pending_status).strategy().target_accepted());
    EXPECT_EQ(call(list).tasks().tasks_size(), 4);
    service.reset();
    TaskDirectory archive;
    const auto backup = archive.path / "portfolio-candidate-backup";
    const auto restore = archive.path / "portfolio-candidate-restored";
    {
        auto exported = research::cold_backup(directory.path, candidate.owner());
        std::vector<backup_files::Snapshot> snapshots;
        snapshots.push_back(std::move(exported.snapshot));
        backup_files::Cut cut(std::move(snapshots));
        const auto plan = cut.preview("portfolio-candidate-independent");
        cut.save("portfolio-candidate-independent", backup, sha256_bytes(plan.dump()));
    }
    backup_files::restore_files(backup, restore);
    std::filesystem::rename(directory.path, archive.path / "preserved-original-research");
    std::filesystem::rename(warehouse.path, archive.path / "preserved-original-data");
    const auto restored_root = restore / "owners/0";
    {
        const auto inspected = research::cold_backup(restored_root, candidate.owner());
        EXPECT_TRUE(
            std::ranges::any_of(inspected.snapshot.dependencies, [](const auto& dependency) {
                return dependency.object.owner().domain() == "data" && !dependency.included;
            })); // Research-only restore must not invent external Data closure.
    }

    // Stop this owned process before the isolated archive directory is removed,
    // including on an assertion failure or exception.
    struct StopRestored
    {
        std::unique_ptr<ChildProcess>& process;

        ~StopRestored()
        {
            process.reset();
        }
    } stop_restored{service};

    launch(true, restored_root);
    wait_for_initialization();
    research::proto::ResearchRequest read_candidate;
    read_candidate.mutable_strategy()->mutable_get()->set_id(candidate.id());
    EXPECT_EQ(
        call(read_candidate).strategy().package().SerializeAsString(),
        candidate.SerializeAsString());
    EXPECT_EQ(call(freeze).strategy().package().SerializeAsString(), candidate.SerializeAsString());
    EXPECT_EQ(
        call(pending_command).strategy().delivery().SerializeAsString(),
        pending_draft.SerializeAsString());
    EXPECT_FALSE(call(pending_status).strategy().target_accepted());
    EXPECT_TRUE(call(delivery_status).strategy().target_accepted());
    EXPECT_TRUE(call(explicit_status).strategy().target_accepted());
    const auto from_restored = completed(explicit_draft.target_task_id().c_str());
    EXPECT_EQ(
        from_restored.result_task().SerializeAsString(),
        explicit_used.result_task().SerializeAsString());
    EXPECT_EQ(
        from_restored.backtest().SerializeAsString(),
        explicit_used.backtest().SerializeAsString());
    EXPECT_EQ(call(explicit_submit).task().attempt(), 1U);
    EXPECT_EQ(call(list).tasks().tasks_size(), 4);
    research::proto::ResearchRequest missing;
    missing.mutable_get()->set_id(pending_draft.target_task_id());
    EXPECT_THROW(call(missing), std::runtime_error);
}
