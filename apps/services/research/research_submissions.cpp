#include "research_host_internal.hpp"

namespace asterion::tasks {

PolledTask<> ResearchHost::submit_backtest(Call& call)
{
    const auto& p = call.request.submit();
    const auto& b = p.backtest();
    if (p.has_research_plan())
        co_await verify_plan(call, p.research_plan());
    std::optional<research::proto::StrategyPackage> candidate;
    auto use = p.strategy_use();
    if (p.has_strategy_use()) {
        const auto digest = sha256_bytes(b.SerializeAsString());
        if (!use.request_sha256().empty() && use.request_sha256() != digest)
            throw Error(ErrorCode::conflict, "strategy delivery request identity mismatch");
        use.set_request_sha256(digest);
        protocol::validate_strategy_use(use);
        const auto node = configuration_.node_id;
        if (use.package().owner().node_id() != node
            || use.package().owner().service_id() != configuration_.service)
            throw Error(ErrorCode::conflict, "strategy delivery belongs to another Research owner");
        co_await wait_for_store({}, call.deadline);
        research::Strategies strategies(*storage_, use.package().owner());
        candidate = strategies.package(use.package().id());
        if (!candidate->has_logic() && !candidate->definition().has_portfolio())
            throw std::invalid_argument(
                "reference delivery requires unchanged fixed backtest reuse");
        strategies.verify_use(use, p.id(), false);
        if (strategies.delivery_status(use.delivery().id()).target_accepted()) {
            const auto task = storage_->describe(p.id());
            if (task.has_retirement()
                || task.strategy_use().SerializeAsString() != use.SerializeAsString()
                || task.research_plan().SerializeAsString()
                    != p.research_plan().SerializeAsString())
                throw Error(
                    ErrorCode::conflict,
                    "strategy delivery target names different fixed conditions");
            auto read = storage_->prepare_input(p.id());
            co_await file_work([&] {
                read.load_for_claim();
            });
            co_await ready(call);
            call.input_in_time();
            const auto& owner = p.data_owner();
            const auto& fixed = read.execution().data_owner();
            if ((!owner.node_id().empty() && owner.node_id() != fixed.node_id())
                || (!owner.service_id().empty() && owner.service_id() != fixed.service_id()))
                throw Error(ErrorCode::conflict, "strategy delivery Data owner changed");
            *call.response.mutable_task() = storage_->describe(p.id());
            co_return;
        }
        strategies.verify_use(use, p.id(), true);
    }
    // A product of the daily table brings its own months.
    if (static_cast<std::size_t>(b.contracts_size()) > max_account_contracts
        || (b.contracts().empty() && std::ranges::none_of(b.series(), [](const auto& series) {
               return series.has_product();
           })))
        throw std::invalid_argument("backtest requires 1 to 256 contracts");
    // Months of a dominant series are resolved together by Data.
    std::vector<bool> in_series(static_cast<std::size_t>(b.contracts_size()));
    for (const auto& series : b.series()) {
        if (series.has_product()) {
            if (series.contracts_size())
                throw std::invalid_argument("a dominant series names its months or its product");
            continue;
        }
        if (series.contracts_size() < 2)
            throw std::invalid_argument("a dominant series requires at least two month contracts");
        for (const auto index : series.contracts()) {
            if (index >= in_series.size() || in_series[index])
                throw std::invalid_argument("invalid dominant series contracts");
            in_series[index] = true;
        }
    }

    // The frozen contracts: ordinary ones in request order, then the dominant
    // months of each series, each with what it trades at.
    struct Frozen
    {
        data::proto::BarDataset dataset;
        const protocol::proto::CostSchedule* costs;
        unsigned slippage;
    };

    std::vector<Frozen> datasets;
    std::size_t total_bars = 0;
    const auto freeze = [&](data::proto::BarDataset dataset,
                            const protocol::proto::CostSchedule& costs,
                            unsigned slippage) {
        total_bars += static_cast<std::size_t>(dataset.bars_size());
        if (total_bars > protocol::max_dataset_bars)
            throw std::invalid_argument("dataset exceeds 200000 bars; narrow the date range");
        datasets.push_back({std::move(dataset), &costs, slippage});
    };
    const auto keep = [&](std::size_t index, data::proto::BarDataset dataset) {
        const auto& contract = b.contracts(static_cast<int>(index));
        freeze(std::move(dataset), contract.cost_schedule(), contract.slippage_ticks());
    };
    for (std::size_t c = 0; c < in_series.size(); ++c) {
        if (in_series[c])
            continue;
        data::proto::DataRequest query;
        *query.mutable_bar_dataset() = b.contracts(static_cast<int>(c)).data();
        auto prepared = co_await data_input(call, std::move(query));
        keep(c, std::move(*prepared.mutable_bar_dataset()));
    }
    std::vector<data::proto::DominantSchedule> schedules;
    for (const auto& series : b.series()) {
        data::proto::DataRequest query;
        if (series.has_product())
            *query.mutable_dominant_series()->mutable_product() = series.product().data();
        for (const auto index : series.contracts())
            *query.mutable_dominant_series()->add_months() = b.contracts(index).data();
        auto prepared = co_await data_input(call, std::move(query));
        auto& resolved = *prepared.mutable_dominant_series();
        if (resolved.months_size() != resolved.datasets_size())
            throw std::invalid_argument("dominant series response has inconsistent months");
        for (auto& roll : *resolved.mutable_schedule()->mutable_rolls())
            roll.set_contract(roll.contract() + static_cast<unsigned>(datasets.size()));
        for (int m = 0; m < resolved.months_size(); ++m) {
            // A product's months are Data's to find; all trade at the product's costs.
            if (series.has_product()) {
                freeze(
                    std::move(*resolved.mutable_datasets(m)),
                    series.product().cost_schedule(),
                    series.product().slippage_ticks());
                continue;
            }
            if (resolved.months(m) >= static_cast<unsigned>(series.contracts_size()))
                throw std::invalid_argument("dominant series response has an unknown month");
            keep(series.contracts(resolved.months(m)), std::move(*resolved.mutable_datasets(m)));
        }
        schedules.push_back(std::move(*resolved.mutable_schedule()));
    }
    backtest::proto::BacktestInput input;
    *input.mutable_data_owner() = call.data_route->owner;
    auto* paper = input.mutable_paper();
    *paper->mutable_deposit() = b.deposit();
    *paper->mutable_risk() = b.risk();
    for (auto& frozen : datasets) {
        auto* contract = paper->add_contracts();
        *contract->mutable_dataset() = std::move(frozen.dataset);
        *contract->mutable_cost_schedule() = *frozen.costs;
        contract->set_slippage_ticks(frozen.slippage);
    }
    for (auto& schedule : schedules)
        *input.add_series() = std::move(schedule);
    *input.mutable_settlement_margins() = b.settlement_margins();
    protocol::set_backtest_strategies(input, b);
    if (!b.margin_snapshot().empty()) {
        if (paper->contracts_size() != 1 || !b.settlement_margins().empty())
            throw std::invalid_argument(
                "historical margins require one contract and no manual settlement events");
        data::proto::DataRequest query;
        auto* margins = query.mutable_historical_margins();
        margins->set_snapshot(b.margin_snapshot());
        margins->set_contract(
            protocol::history_identity(paper->contracts(0).dataset().contract()).key());
        for (const auto& day : paper->contracts(0).dataset().days())
            margins->add_trading_days(day.trading_day());
        const auto prepared = co_await data_input(call, std::move(query));
        if (prepared.historical_margins().snapshot() != b.margin_snapshot())
            throw std::invalid_argument("historical margin snapshot is unavailable");
        protocol::apply_historical_margins(input, prepared.historical_margins());
    }

    co_await ready(call);
    call.input_in_time();
    refuse_during_upgrade(quiescing_);
    const auto programme = co_await program(call);
    call.input_in_time();
    *call.response.mutable_task() = (co_await submit(call, [&] {
        input.set_dataset_revision(protocol::dataset_revision(input.paper()));
        auto submission = task_submission(*storage_, p.id(), std::move(input));
        set_program_artifact(submission, programme.artifact);
        if (p.has_research_plan())
            set_research_plan(*storage_, submission, p.research_plan());
        if (candidate)
            set_strategy_use(*storage_, submission, use, candidate->definition());
        return submission;
    })).task();
}

PolledTask<> ResearchHost::reuse_backtest(Call& call)
{
    const auto& p = call.request.submit();
    const auto& reuse = p.backtest_reuse();
    if (reuse.has_slippage_ticks() && reuse.has_fee_multiplier())
        throw std::invalid_argument("fee and slippage scenarios require separate requests");
    using Choice = backtest::proto::BacktestReuse;
    if (p.id() == reuse.source_id())
        throw std::invalid_argument("experiment reuse requires a new task id");
    if ((reuse.choice() != Choice::FIXED && reuse.choice() != Choice::NEW_SNAPSHOT)
        || (reuse.choice() == Choice::FIXED ? !reuse.snapshot().empty()
                                            : reuse.snapshot().size() != 64
                    || reuse.snapshot().find_first_not_of("0123456789abcdef") != std::string::npos))
        throw std::invalid_argument("explicit experiment data choice required");
    if ((reuse.has_slippage_ticks() || reuse.has_fee_multiplier())
        && reuse.choice() != Choice::FIXED)
        throw std::invalid_argument("cost sensitivity requires the original fixed data");
    std::optional<research::proto::StrategyPackage> candidate;
    if (p.has_strategy_use()) {
        if (reuse.choice() != Choice::FIXED
            || (reuse.has_slippage_ticks() || reuse.has_fee_multiplier()))
            throw std::invalid_argument("strategy delivery requires unchanged fixed evidence");
        const auto& use = p.strategy_use();
        protocol::validate_strategy_use(use);
        const auto node = configuration_.node_id;
        if (use.package().owner().node_id() != node
            || use.package().owner().service_id() != configuration_.service)
            throw Error(ErrorCode::conflict, "strategy delivery belongs to another Research owner");
        co_await wait_for_store({}, call.deadline);
        research::Strategies strategies(*storage_, use.package().owner());
        strategies.verify_use(use, p.id(), false);
        candidate = strategies.package(use.package().id());
        if (candidate->has_logic())
            throw std::invalid_argument(
                "logic delivery requires a new explicit backtest configuration");
        const auto draft = strategies.delivery(use.delivery().id());
        if (draft.reference_result().id() != reuse.source_id()
            || draft.reference_result().version() != reuse.source_result_digest()
            || draft.source_input_sha256() != reuse.source_input_sha256())
            throw Error(ErrorCode::conflict, "strategy delivery source evidence mismatch");
        if (strategies.delivery_status(draft.id()).target_accepted()) {
            *call.response.mutable_task() = storage_->describe(p.id());
            co_return; // Existing admission survives deprecation and a disconnected Data owner.
        }
        strategies.verify_use(use, p.id(), true);
        const auto programme = co_await program(call);
        if (programme.artifact != candidate->definition().program_artifact())
            throw Error(
                ErrorCode::conflict,
                "strategy delivery requires its fixed execution program");
    }
    co_await wait_for_store(reuse.source_id());
    auto source_slot = verification_slots_.acquire(false, reuse.source_id());
    auto read = storage_->prepare_result(reuse.source_id());
    co_await file_work([&] {
        read.verify();
    });
    co_await ready(call);
    call.input_in_time();
    auto verified = storage_->confirm_result(std::move(read));
    const auto& task = verified.result_task();
    const auto source_program = task.program_artifact();
    if (task.kind() != wire::BACKTEST || !task.has_input() || !verified.has_backtest())
        throw std::invalid_argument("backtest reuse requires a compatible backtest result");
    if (task.result_digest() != reuse.source_result_digest())
        throw std::invalid_argument("experiment evidence changed; reopen its result before reuse");
    if (reuse.choice() == Choice::FIXED) {
        const auto owner = task.input().data_owner();
        require_fixed_owner(p.data_owner(), owner);
        *call.request.mutable_submit()->mutable_data_owner() = owner;
    }
    backtest::proto::BacktestInput original, input;
    std::string original_bytes;
    co_await file_work([&] {
        if ((reuse.has_slippage_ticks() || reuse.has_fee_multiplier())
            && task.risk_artifact() != risk_providers::Module::selected().artifact())
            throw std::invalid_argument(
                "cost sensitivity requires the original risk implementation");
        if (candidate) {
            const auto definition = protocol::strategy_definition(
                task.input(),
                task.program_artifact(),
                task.risk_artifact());
            if (definition.SerializeAsString() != candidate->definition().SerializeAsString()
                || task.risk_artifact() != risk_providers::Module::selected().artifact())
                throw Error(
                    ErrorCode::conflict,
                    "strategy delivery definition or risk implementation changed");
        }
        original = std::move(*verified.mutable_result_task()->mutable_input());
        original_bytes = original.SerializeAsString();
        if (sha256_bytes(original_bytes) != reuse.source_input_sha256())
            throw std::invalid_argument(
                "experiment evidence changed; reopen its result before reuse");
        verified.Clear();
        if (reuse.choice() == Choice::FIXED)
            input = std::move(original);
        else {
            input = original;
            input.mutable_paper()->clear_contracts();
            input.clear_series();
        }
    });
    co_await ready(call);
    call.input_in_time();
    const auto selected = [&](const data::proto::BarDataset& dataset) {
        auto selection = protocol::bar_dataset_selection(dataset);
        if (reuse.choice() == Choice::NEW_SNAPSHOT)
            selection.mutable_daily()->set_snapshot(reuse.snapshot());
        return selection;
    };
    const auto keep = [&](const protocol::proto::PaperContract& source,
                          data::proto::BarDataset dataset) {
        if (dataset.contract().SerializeAsString()
            != source.dataset().contract().SerializeAsString())
            throw std::invalid_argument(
                "new snapshot changes contract specifications; configure a new backtest");
        auto* target = input.mutable_paper()->add_contracts();
        *target->mutable_cost_schedule() = source.cost_schedule();
        target->set_slippage_ticks(source.slippage_ticks());
        *target->mutable_dataset() = std::move(dataset);
    };
    if (reuse.choice() == Choice::FIXED) {
        for (int c = 0; c < input.paper().contracts_size(); ++c) {
            data::proto::DataRequest query;
            *query.mutable_bar_dataset() = selected(input.paper().contracts(c).dataset());
            auto reply = co_await data_input(call, std::move(query));
            *input.mutable_paper()->mutable_contracts(c)->mutable_dataset() =
                std::move(*reply.mutable_bar_dataset());
        }
    } else {
        // Preserve the original actual-month universe and each month's own costs.
        // A new snapshot can change the schedule, not invent costs for new months.
        input.mutable_paper()->clear_contracts();
        input.clear_series();
        std::vector<bool> in_series(original.paper().contracts_size());
        std::vector<std::vector<unsigned>> members;
        for (const auto& schedule : original.series()) {
            std::set<unsigned> indexes;
            for (const auto& roll : schedule.rolls())
                indexes.insert(roll.contract());
            members.emplace_back(indexes.begin(), indexes.end());
            for (const auto index : indexes)
                in_series[index] = true;
        }
        for (int c = 0; c < original.paper().contracts_size(); ++c) {
            if (in_series[c])
                continue;
            data::proto::DataRequest query;
            *query.mutable_bar_dataset() = selected(original.paper().contracts(c).dataset());
            auto reply = co_await data_input(call, std::move(query));
            keep(original.paper().contracts(c), std::move(*reply.mutable_bar_dataset()));
        }
        for (std::size_t group = 0; group < members.size(); ++group) {
            const auto& indexes = members[group];
            data::proto::DataRequest query;
            query.mutable_dominant_series()->set_first_day(
                original.series(static_cast<int>(group)).rolls(0).trading_day());
            for (const auto index : indexes)
                *query.mutable_dominant_series()->add_months() =
                    selected(original.paper().contracts(index).dataset());
            auto reply = co_await data_input(call, std::move(query));
            auto& resolved = *reply.mutable_dominant_series();
            if (resolved.months_size() != resolved.datasets_size())
                throw std::invalid_argument("dominant series response has inconsistent months");
            const auto offset = static_cast<unsigned>(input.paper().contracts_size());
            for (int m = 0; m < resolved.months_size(); ++m) {
                if (resolved.months(m) >= indexes.size())
                    throw std::invalid_argument("dominant series response has an unknown month");
                keep(
                    original.paper().contracts(indexes[resolved.months(m)]),
                    std::move(*resolved.mutable_datasets(m)));
            }
            for (auto& roll : *resolved.mutable_schedule()->mutable_rolls())
                roll.set_contract(roll.contract() + offset);
            *input.add_series() = std::move(*resolved.mutable_schedule());
        }
    }
    co_await ready(call);
    call.input_in_time();
    refuse_during_upgrade(quiescing_);
    const auto programme = co_await program(call);
    call.input_in_time();
    if ((reuse.has_slippage_ticks() || reuse.has_fee_multiplier())
        && programme.artifact != source_program)
        throw Error(
            ErrorCode::conflict,
            "cost sensitivity requires its original execution program");
    *call.response.mutable_task() = (co_await submit(call, [&] {
        if (reuse.choice() == Choice::NEW_SNAPSHOT)
            *input.mutable_data_owner() = call.data_route->owner;
        input.set_dataset_revision(protocol::dataset_revision(input.paper()));
        if (reuse.choice() == Choice::FIXED && input.SerializeAsString() != original_bytes)
            throw std::invalid_argument(
                "reused backtest input does not match original fixed evidence");
        if (reuse.has_slippage_ticks() || reuse.has_fee_multiplier())
            input = reuse.has_fee_multiplier()
                ? protocol::backtest_fee_variant(
                      input,
                      Decimal::from_raw(reuse.fee_multiplier().units()))
                : protocol::backtest_slippage_variant(input, reuse.slippage_ticks());
        auto submission = task_submission(*storage_, p.id(), std::move(input));
        set_program_artifact(submission, programme.artifact);
        if (candidate)
            set_strategy_use(*storage_, submission, p.strategy_use(), candidate->definition());
        wire::TaskDerivation source;
        source.set_source_id(reuse.source_id());
        source.set_source_result_digest(reuse.source_result_digest());
        source.set_data_choice(reuse.choice() == Choice::FIXED ? "fixed" : "snapshot");
        source.set_snapshot(reuse.snapshot());
        submission.set_derivation(std::move(source));
        return submission;
    })).task();
}

PolledTask<> ResearchHost::verify_plan(Call& call, const research::proto::ProjectBinding& binding)
{
    co_await wait_for_store({}, call.deadline);
    call.input_in_time();
    research::Projects(*storage_, binding.owner()).verify_binding(binding);
}

PolledTask<research::proto::VariableDefinition>
ResearchHost::resolve_variable(Call& call, resource::proto::Object object)
{
    protocol::validate_variable_reference(object);
    co_await wait_for_store({}, call.deadline);
    call.input_in_time();
    const auto definition = research::Variables(*storage_, object.owner()).definition(object.id());
    protocol::verify_variable_reference(object, definition);
    co_return definition;
}

PolledTask<> ResearchHost::submit_factor(Call& call)
{
    const auto& p = call.request.submit();
    const auto& f = p.factor_request();
    std::optional<research::proto::ProjectBinding> binding;
    if (p.has_research_plan())
        binding = p.research_plan();
    if (p.has_derivation()) {
        co_await wait_for_store(p.derivation().source_id());
        auto source_slot = verification_slots_.acquire(false, p.derivation().source_id());
        auto read = storage_->prepare_result(p.derivation().source_id());
        co_await file_work([&] {
            read.verify();
        });
        co_await ready(call);
        auto source = storage_->confirm_result(std::move(read));
        if (!binding && source.result_task().has_research_plan())
            binding = source.result_task().research_plan();
        co_await file_work([&] {
            if (!source.has_factor()
                || source.result_task().result_digest() != p.derivation().source_result_digest())
                throw std::invalid_argument("invalid experiment derivation");
            const auto parameters = protocol::task_execution(source.result_task(), "");
            const auto expected = protocol::reuse_daily_factor(
                parameters.factor(),
                source.result_task().factor().dataset_revision(),
                p.derivation().snapshot(),
                std::vector<unsigned>(f.lookbacks().begin(), f.lookbacks().end()));
            if (expected.SerializeAsString() != f.SerializeAsString())
                throw std::invalid_argument("invalid experiment derivation");
        });
        co_await ready(call);
        if (p.derivation().data_choice() == "fixed") {
            const auto owner = source.result_task().factor().data_owner();
            require_fixed_owner(p.data_owner(), owner);
            *call.request.mutable_submit()->mutable_data_owner() = owner;
        }
    }
    if (binding)
        co_await verify_plan(call, *binding);
    std::optional<research::proto::VariableDefinition> variable;
    if (f.has_variable_definition())
        variable = co_await resolve_variable(call, f.variable_definition());
    auto input = protocol::factor_input(f);
    if (variable) {
        if (f.lookbacks_size() != 1 || f.lookbacks(0) != variable->window())
            throw std::invalid_argument("factor window differs from its fixed variable definition");
        *input.mutable_variable_definition() = *variable;
    }
    for (const auto& source : f.series()) {
        auto prepared = co_await data_input(call, protocol::factor_series_query(source));
        for (auto& series : protocol::factor_series(source, std::move(prepared)))
            protocol::add_factor_series(input, std::move(series));
    }
    co_await ready(call);
    call.input_in_time();
    refuse_during_upgrade(quiescing_);
    *input.mutable_data_owner() = call.data_route->owner;
    const auto programme = co_await program(call);
    call.input_in_time();
    if (variable && variable->owner().SerializeAsString() != programme.owner.SerializeAsString())
        throw Error(ErrorCode::conflict, "research variable owner mismatch");
    if (binding && binding->owner().SerializeAsString() != programme.owner.SerializeAsString())
        throw Error(ErrorCode::conflict, "research project owner mismatch");
    *call.response.mutable_task() = (co_await submit(call, [&] {
        // Hashing a daily series is file-pool work, like the rest of the submission.
        input.set_dataset_revision(protocol::factor_revision(input.series()));
        if (!f.expected_dataset_revision().empty()
            && input.dataset_revision() != f.expected_dataset_revision())
            throw std::invalid_argument("reused factor data does not match the original revision");
        auto submission = task_submission(*storage_, p.id(), std::move(input));
        set_program_artifact(submission, programme.artifact);
        if (binding)
            set_research_plan(*storage_, submission, *binding);
        if (p.has_derivation())
            submission.set_derivation(p.derivation());
        return submission;
    })).task();
}

PolledTask<ResearchProgram> ResearchHost::program(const Call& call)
{
    const auto observed = co_await data_.program(
        configuration_.service,
        static_cast<std::uint64_t>(::getpid()),
        5s,
        call.request.correlation_id());
    if (loaded_program_
        && (loaded_program_->owner.SerializeAsString() != observed.owner.SerializeAsString()
            || loaded_program_->artifact != observed.artifact))
        throw Error(ErrorCode::conflict, "loaded research program identity changed");
    loaded_program_ = observed;
    if (!program_preservation_.valid()) {
        program_preservation_ =
            files_
                .submit([this, observed](std::stop_token stop) {
                    (void)programs::Files(configuration_.directory / "programs", observed.owner)
                        .preserve(current_executable(), observed.artifact, stop);
                })
                .share();
    }
    const auto preserved = program_preservation_;
    co_await PollUntil{[&] {
        return preserved.wait_for(0ms) == std::future_status::ready;
    }};
    preserved.get();
    co_return observed;
}

PolledTask<research::proto::AccountReviewInput> ResearchHost::account_evidence(
    const Call& call,
    const research::proto::AccountReviewRequest& selected,
    const resource::proto::Owner& owner,
    const std::string& artifact)
{
    protocol::validate_account_review_request(selected);
    const auto route =
        co_await data_.resolve_facts(selected.source().owner(), 5s, call.request.correlation_id());
    protocol::proto::Request query;
    query.set_version(1);
    query.set_session_id(route.owner.service_id());
    query.set_correlation_id(next_correlation_id());
    *query.mutable_facts()->mutable_read() = selected.source();
    const auto raw = co_await data_.read(route, query.SerializeAsString(), 30s);
    research::proto::AccountReviewInput result;
    co_await file_work([&] {
        protocol::proto::Response reply;
        if (!reply.ParseFromString(*raw))
            throw Error(ErrorCode::unavailable, "invalid trading facts response");
        protocol::validate_message(reply);
        if (reply.version() != query.version() || reply.session_id() != query.session_id()
            || reply.correlation_id() != query.correlation_id())
            throw Error(ErrorCode::conflict, "trading facts response identity mismatch");
        if (reply.has_error())
            throw_remote_error(reply.error().code(), reply.error().message());
        if (!reply.has_facts())
            throw Error(ErrorCode::unavailable, "fixed trading facts are unavailable");
        result = protocol::make_account_review_input(owner, selected, reply.facts(), artifact);
    });
    call.input_in_time();
    co_return result;
}

PolledTask<> ResearchHost::submit_logic_trial(Call& call)
{
    const auto& submission = call.request.submit();
    const auto& selected = submission.logic_trial().example();
    protocol::validate_logic_object(selected, "logic_case");
    validate_id(submission.id());
    if (submission.has_data_owner() || submission.has_derivation() || submission.has_research_plan()
        || submission.has_strategy_use())
        throw std::invalid_argument("logic trial does not accept Data or backtest bindings");
    std::optional<wire::Task> existing = storage_->find_task(submission.id());
    if (existing) {
        if (existing->kind() != wire::LOGIC_TRIAL || existing->has_retirement())
            throw Error(ErrorCode::conflict, "logic trial identity is already used or retired");
        auto read = storage_->prepare_input(submission.id());
        co_await file_work([&] {
            read.load_for_claim();
        });
        co_await ready(call);
        call.input_in_time();
        if (!read.execution().has_logic_trial()
            || protocol::logic_object(read.execution().logic_trial().example()).SerializeAsString()
                != selected.SerializeAsString())
            throw Error(ErrorCode::conflict, "logic trial identity names different fixed evidence");
        *call.response.mutable_task() = storage_->describe(submission.id());
        co_return;
    }
    const auto programme = co_await program(call);
    co_await wait_for_store({}, call.deadline);
    call.input_in_time();
    auto input = research::Logic(*storage_, programme.owner).input(selected, true);
    *call.response.mutable_task() = (co_await submit(call, [&] {
        auto accepted = task_submission(*storage_, submission.id(), std::move(input));
        set_program_artifact(accepted, programme.artifact);
        return accepted;
    })).task();
}

PolledTask<> ResearchHost::submit_sample(Call& call)
{
    const auto& submission = call.request.submit();
    const auto& selected = submission.sample();
    protocol::validate_sample_request(selected);
    if (submission.has_research_plan())
        co_await verify_plan(call, submission.research_plan());
    validate_id(submission.id());
    if (submission.has_derivation())
        throw std::invalid_argument("sample does not accept client derivation");
    std::optional<wire::Task> existing = storage_->find_task(submission.id());
    if (existing) {
        if (existing->kind() != wire::SAMPLE || existing->has_retirement())
            throw Error(ErrorCode::conflict, "sample identity is already used or retired");
        auto read = storage_->prepare_input(submission.id());
        co_await file_work([&] {
            read.load_for_claim();
        });
        co_await ready(call);
        call.input_in_time();
        const auto& requested_owner = submission.data_owner();
        const auto& fixed_owner = read.execution().data_owner();
        if (!read.execution().has_sample()
            || read.execution().sample().request().SerializeAsString()
                != selected.SerializeAsString()
            || existing->research_plan().SerializeAsString()
                != submission.research_plan().SerializeAsString()
            || (!requested_owner.node_id().empty()
                && requested_owner.node_id() != fixed_owner.node_id())
            || (!requested_owner.service_id().empty()
                && requested_owner.service_id() != fixed_owner.service_id()))
            throw Error(ErrorCode::conflict, "sample identity names different fixed evidence");
        *call.response.mutable_task() = storage_->describe(submission.id());
        co_return; // Replay needs neither current Data nor a replacement executable.
    }
    data::proto::DataRequest request;
    *request.mutable_bar_dataset() = selected.selection();
    auto source = co_await data_input(call, std::move(request));
    const auto programme = co_await program(call);
    call.input_in_time();
    research::proto::SampleInput input;
    *input.mutable_owner() = programme.owner;
    *input.mutable_data_owner() = call.data_route->owner;
    *input.mutable_request() = selected;
    *input.mutable_dataset() = std::move(*source.mutable_bar_dataset());
    *call.response.mutable_task() = (co_await submit(call, [&] {
        auto accepted = task_submission(*storage_, submission.id(), std::move(input));
        set_program_artifact(accepted, programme.artifact);
        if (submission.has_research_plan())
            set_research_plan(*storage_, accepted, submission.research_plan());
        return accepted;
    })).task();
}

PolledTask<> ResearchHost::submit_evaluation(Call& call)
{
    const auto& submission = call.request.submit();
    const auto& selected = submission.evaluation();
    protocol::validate_evaluation_request(selected);
    validate_id(submission.id());
    if (submission.has_data_owner() || submission.has_derivation() || submission.has_research_plan()
        || submission.has_strategy_use())
        throw std::invalid_argument(
            "evaluation does not accept Data selection or execution bindings");
    if (const auto found_task = storage_->find_task(submission.id())) {
        const auto& existing = *found_task;
        if (existing.kind() != wire::PERFORMANCE_EVALUATION || existing.has_retirement())
            throw Error(ErrorCode::conflict, "evaluation identity is already used or retired");
        auto read = storage_->prepare_input(submission.id());
        co_await file_work([&] {
            read.load_for_claim();
        });
        co_await ready(call);
        if (!read.execution().has_evaluation()
            || read.execution().evaluation().request().SerializeAsString()
                != selected.SerializeAsString())
            throw Error(ErrorCode::conflict, "evaluation identity names different fixed evidence");
        *call.response.mutable_task() = storage_->describe(submission.id());
        co_return; // Existing receipt never recalculates or reselects a source.
    }
    const auto observed = co_await program(call);
    auto slot = verification_slots_.acquire(false, selected.source().id());
    auto read = storage_->prepare_result(selected.source().id());
    const auto input_digest = read.input_digest();
    const auto input_bytes = read.input_bytes();
    co_await file_work([&] {
        read.verify();
    });
    co_await ready(call);
    call.input_in_time();
    const auto source = storage_->confirm_result(std::move(read));
    research::proto::EvaluationInput input;
    co_await file_work([&] {
        input =
            capture_evaluation_input(observed.owner, selected, source, input_digest, input_bytes);
    });
    *call.response.mutable_task() = (co_await submit(call, [&] {
        auto accepted = task_submission(*storage_, submission.id(), std::move(input));
        set_program_artifact(accepted, observed.artifact);
        return accepted;
    })).task();
}

PolledTask<> ResearchHost::submit_statistics(Call& call)
{
    const auto& submission = call.request.submit();
    const auto& selected = submission.statistics();
    protocol::validate_statistics_request(selected);
    if (submission.has_research_plan())
        co_await verify_plan(call, submission.research_plan());
    validate_id(submission.id());
    if (submission.has_data_owner() || submission.has_derivation())
        throw std::invalid_argument("statistics do not accept Data selection or client derivation");
    if (const auto found_task = storage_->find_task(submission.id())) {
        const auto& existing = *found_task;
        if (existing.kind() != wire::STATISTICS || existing.has_retirement())
            throw Error(ErrorCode::conflict, "statistics identity is already used or retired");
        auto read = storage_->prepare_input(submission.id());
        co_await file_work([&] {
            read.load_for_claim();
        });
        co_await ready(call);
        if (existing.research_plan().SerializeAsString()
                != submission.research_plan().SerializeAsString()
            || !read.execution().has_statistics()
            || read.execution().statistics().request().SerializeAsString()
                != selected.SerializeAsString())
            throw Error(ErrorCode::conflict, "statistics identity names different fixed evidence");
        *call.response.mutable_task() = storage_->describe(submission.id());
        co_return;
    }
    const auto observed = co_await program(call);
    auto slot = verification_slots_.acquire(false, selected.source().id());
    auto read = storage_->prepare_result(selected.source().id());
    const auto input_digest = read.input_digest();
    const auto input_bytes = read.input_bytes();
    co_await file_work([&] {
        read.verify();
    });
    co_await ready(call);
    call.input_in_time();
    const auto source = storage_->confirm_result(std::move(read));
    auto input =
        capture_statistics_input(observed.owner, selected, source, input_digest, input_bytes);
    *call.response.mutable_task() = (co_await submit(call, [&] {
        auto accepted = task_submission(*storage_, submission.id(), std::move(input));
        set_program_artifact(accepted, observed.artifact);
        if (submission.has_research_plan())
            set_research_plan(*storage_, accepted, submission.research_plan());
        return accepted;
    })).task();
}

PolledTask<> ResearchHost::submit_account_review(Call& call)
{
    const auto& submission = call.request.submit();
    const auto& selected = submission.account_review();
    protocol::validate_account_review_request(selected);
    validate_id(submission.id());
    if (submission.has_data_owner() || submission.has_derivation())
        throw std::invalid_argument(
            "account review does not accept Data selection or experiment derivation");
    if (const auto found_task = storage_->find_task(submission.id())) {
        const auto& existing = *found_task;
        if (existing.kind() != wire::ACCOUNT_REVIEW || existing.has_retirement())
            throw Error(ErrorCode::conflict, "account review identity is already used or retired");
        auto read = storage_->prepare_input(submission.id());
        co_await file_work([&] {
            read.load_for_claim();
        });
        co_await ready(call);
        if (!read.execution().has_account_review()
            || read.execution().account_review().request().SerializeAsString()
                != selected.SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "account review identity names different fixed evidence");
        *call.response.mutable_task() = storage_->describe(submission.id());
        co_return; // Replaying accepted identity needs neither source nor current program.
    }
    const auto observed = co_await program(call);
    auto input = co_await account_evidence(call, selected, observed.owner, observed.artifact);
    *call.response.mutable_task() = (co_await submit(call, [&] {
        return task_submission(*storage_, submission.id(), std::move(input));
    })).task();
}

} // namespace asterion::tasks
