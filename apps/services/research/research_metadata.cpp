#include "research_host_internal.hpp"

namespace asterion::tasks {

PolledTask<> ResearchHost::variable_operation(Call& call)
{
    const auto node = configuration_.node_id;
    co_await wait_for_store({}, call.deadline);
    call.input_in_time();
    const auto request = call.request.variable();
    const auto catalogue = call.request.resources();
    if (request.has_mutation())
        refuse_during_upgrade(quiescing_);
    resource::proto::Owner owner;
    owner.set_node_id(node);
    owner.set_service_id(configuration_.service);
    owner.set_domain("research");
    auto work = std::make_shared<MetadataWork>();
    metadata_work_ = work;
    try {
        work->result = journal_.submit([this, request, catalogue, owner, work](std::stop_token) {
            if (request.has_preview()) {
                *work->variable.mutable_preview() = research::Variables::preview(request.preview());
                return;
            }
            research::Variables variables(
                *storage_,
                owner,
                request.has_mutation() ? sqlite::Database::Access::writer
                                       : sqlite::Database::Access::read_only);
            if (!catalogue.kind().empty()) {
                work->catalogue = variables.resources(catalogue);
                return;
            }
            if (request.has_mutation())
                work->variable = variables.apply(request.mutation());
            else if (request.has_get())
                *work->variable.mutable_definition() = variables.definition(request.get().id());
            else if (request.has_list()) {
                const auto& page = request.list();
                const auto rows = variables.list(page.after(), page.limit());
                for (const auto& row : rows)
                    *work->variable.add_definitions() = row;
                if (rows.size() == page.limit() && !variables.list(rows.back().id(), 1).empty())
                    work->variable.set_next_after(rows.back().id());
            } else
                throw std::invalid_argument("invalid research variable request");
        });
    } catch (...) {
        metadata_work_.reset();
        throw;
    }
    co_await PollUntil{[&] {
        return work->done;
    }};
    if (work->error)
        std::rethrow_exception(work->error);
    if (call.request.has_resources())
        *call.response.mutable_resources() = std::move(work->catalogue);
    else
        *call.response.mutable_variable() = std::move(work->variable);
}

PolledTask<> ResearchHost::logic_operation(Call& call)
{
    const auto node = configuration_.node_id;
    co_await wait_for_store({}, call.deadline);
    call.input_in_time();
    const auto request = call.request.logic();
    const auto catalogue = call.request.resources();
    if (request.has_mutation())
        refuse_during_upgrade(quiescing_);
    resource::proto::Owner owner;
    owner.set_node_id(node);
    owner.set_service_id(configuration_.service);
    owner.set_domain("research");
    auto work = std::make_shared<MetadataWork>();
    metadata_work_ = work;
    try {
        work->result = journal_.submit([this, request, catalogue, owner, work](std::stop_token) {
            research::Logic logic(
                *storage_,
                owner,
                request.has_mutation() ? sqlite::Database::Access::writer
                                       : sqlite::Database::Access::read_only);
            if (!catalogue.kind().empty()) {
                work->catalogue = logic.resources(catalogue);
                return;
            }
            if (request.has_mutation())
                work->logic = logic.apply(request.mutation());
            else if (request.has_get())
                *work->logic.mutable_definition() = logic.definition(request.get().id());
            else if (request.has_example())
                *work->logic.mutable_example() = logic.example(request.example().id());
            else if (request.has_list()) {
                const auto& page = request.list();
                const auto rows = logic.definitions(page.after(), page.limit());
                for (const auto& row : rows)
                    *work->logic.add_definitions() = row;
                if (rows.size() == page.limit() && !logic.definitions(rows.back().id(), 1).empty())
                    work->logic.set_next_after(rows.back().id());
            } else if (request.has_examples()) {
                const auto& page = request.examples();
                const auto rows = logic.examples(page.definition_id(), page.after(), page.limit());
                for (const auto& row : rows)
                    *work->logic.add_examples() = row;
                if (rows.size() == page.limit()
                    && !logic.examples(page.definition_id(), rows.back().id(), 1).empty())
                    work->logic.set_next_after(rows.back().id());
            } else
                throw std::invalid_argument("invalid research logic request");
        });
    } catch (...) {
        metadata_work_.reset();
        throw;
    }
    co_await PollUntil{[&] {
        return work->done;
    }};
    if (work->error)
        std::rethrow_exception(work->error);
    if (call.request.has_resources())
        *call.response.mutable_resources() = std::move(work->catalogue);
    else
        *call.response.mutable_logic() = std::move(work->logic);
}

PolledTask<> ResearchHost::strategy_operation(Call& call)
{
    const auto node = configuration_.node_id;
    co_await wait_for_store({}, call.deadline);
    call.input_in_time();
    const auto request = call.request.strategy();
    const auto catalogue = call.request.resources();
    if (request.has_mutation())
        refuse_during_upgrade(quiescing_);
    resource::proto::Owner owner;
    owner.set_node_id(node);
    owner.set_service_id(configuration_.service);
    owner.set_domain("research");
    // Replay the durable receipt before touching original files.
    if (request.has_mutation()
        && (request.mutation().has_freeze() || request.mutation().has_logic())) {
        auto replay = std::make_shared<MetadataWork>();
        metadata_work_ = replay;
        try {
            replay->result = journal_.submit([this, request, owner, replay](std::stop_token) {
                research::Strategies strategies(*storage_, owner);
                if (const auto receipt = strategies.receipt(request.mutation())) {
                    replay->strategy = *receipt;
                    replay->replay = true;
                }
            });
        } catch (...) {
            metadata_work_.reset();
            throw;
        }
        co_await PollUntil{[&] {
            return replay->done;
        }};
        if (replay->error)
            std::rethrow_exception(replay->error);
        if (replay->replay) {
            *call.response.mutable_strategy() = std::move(replay->strategy);
            co_return;
        }
    }
    std::optional<research::proto::StrategyPackage> captured;
    if (request.has_mutation()
        && (request.mutation().has_freeze() || request.mutation().has_logic())) {
        const auto& source = request.mutation().has_logic() ? request.mutation().logic().source()
                                                            : request.mutation().freeze().source();
        validate_id(source.id());
        co_await wait_for_store({}, call.deadline);
        auto slot = verification_slots_.acquire(false, source.id());
        auto read = storage_->prepare_result(source.id());
        const auto input_digest = read.input_digest();
        co_await file_work([&] {
            read.verify();
        });
        co_await ready(call);
        call.input_in_time();
        auto verified = storage_->confirm_result(std::move(read));
        co_await file_work([&] {
            if (request.mutation().has_logic()) {
                captured = research::capture_logic_strategy_package(
                    owner,
                    request.mutation().logic(),
                    verified,
                    input_digest);
                programs::Files programs(configuration_.directory / "programs", owner);
                (void)programs.find(captured->definition().program_artifact());
                return;
            }
            captured = research::capture_strategy_package(
                owner,
                request.mutation().freeze(),
                verified,
                input_digest);
            programs::Files programs(configuration_.directory / "programs", owner);
            (void)programs.find(captured->definition().program_artifact());
        });
    }
    co_await wait_for_store({}, call.deadline);
    call.input_in_time();
    if (request.has_mutation())
        refuse_during_upgrade(quiescing_);
    auto work = std::make_shared<MetadataWork>();
    metadata_work_ = work;
    try {
        work->result =
            journal_.submit([this, request, catalogue, owner, captured, work](std::stop_token) {
                research::Strategies strategies(
                    *storage_,
                    owner,
                    request.has_mutation() ? sqlite::Database::Access::writer
                                           : sqlite::Database::Access::read_only);
                if (!catalogue.kind().empty()) {
                    work->catalogue = strategies.resources(catalogue);
                } else if (request.has_mutation()) {
                    work->strategy =
                        strategies.apply(request.mutation(), captured ? &*captured : nullptr);
                } else if (request.has_get()) {
                    *work->strategy.mutable_package() = strategies.package(request.get().id());
                } else if (request.has_delivery()) {
                    work->strategy = strategies.delivery_status(request.delivery().id());
                } else if (request.has_list()) {
                    const auto& page = request.list();
                    if (!page.package_id().empty())
                        throw std::invalid_argument("invalid research strategy list query");
                    const auto rows = strategies.packages(page.after(), page.limit());
                    for (const auto& row : rows)
                        *work->strategy.add_packages() = row;
                    if (rows.size() == page.limit()
                        && !strategies.packages(rows.back().id(), 1).empty())
                        work->strategy.set_next_after(rows.back().id());
                } else if (request.has_deliveries()) {
                    const auto& page = request.deliveries();
                    const auto rows =
                        strategies.deliveries(page.package_id(), page.after(), page.limit());
                    for (const auto& row : rows)
                        *work->strategy.add_deliveries() = row;
                    if (rows.size() == page.limit()
                        && !strategies.deliveries(page.package_id(), rows.back().id(), 1).empty())
                        work->strategy.set_next_after(rows.back().id());
                } else {
                    throw std::invalid_argument("invalid research strategy request");
                }
            });
    } catch (...) {
        metadata_work_.reset();
        throw;
    }
    co_await PollUntil{[&] {
        return work->done;
    }};
    if (work->error)
        std::rethrow_exception(work->error);
    if (call.request.has_resources())
        *call.response.mutable_resources() = std::move(work->catalogue);
    else
        *call.response.mutable_strategy() = std::move(work->strategy);
}

PolledTask<> ResearchHost::strategy_holders(Call& call, wire::TaskRetirement& preview)
{
    if ((preview.task().kind() != wire::BACKTEST && preview.task().kind() != wire::LOGIC_TRIAL
            && preview.task().kind() != wire::FACTOR)
        || preview.state() == wire::TaskRetirement::RETIRED)
        co_return;
    co_await wait_for_store({}, call.deadline);
    const auto owner = preview.object().owner();
    const auto id = preview.task().id();
    auto work = std::make_shared<MetadataWork>();
    metadata_work_ = work;
    try {
        work->result = journal_.submit([this, owner, id, work](std::stop_token) {
            research::Strategies strategies(*storage_, owner);
            work->consumers = strategies.source_consumers(id);
        });
    } catch (...) {
        metadata_work_.reset();
        throw;
    }
    co_await PollUntil{[&] {
        return work->done;
    }};
    if (work->error)
        std::rethrow_exception(work->error);
    for (const auto& consumer : work->consumers)
        preview.add_consumers(consumer);
    if (!work->consumers.empty())
        preview.set_state(wire::TaskRetirement::BLOCKED);
}

} // namespace asterion::tasks
