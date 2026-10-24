#include <set>
#include "analytics.hpp"
#include <asterion/domain/trading_days.hpp>
#include <asterion/kernel/environment.hpp>
#include "research_export.hpp"
#include "evaluation_export.hpp"
#include "batch_assessment_export.hpp"
#include "application_impl.hpp"
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/research_project.hpp>
#include <asterion/protocol/research_statistics.hpp>
#include <asterion/protocol/research_evaluation.hpp>
#include <asterion/protocol/research_sample.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/research_logic.hpp>
#include <asterion/protocol/research_batch.hpp>
#include <asterion/protocol/research_assessment.hpp>
#include <asterion/protocol/research_model.hpp>
#include <asterion/protocol/account_review.hpp>
#include <asterion/protocol/notice_review.hpp>
#include <charconv>

namespace asterion::terminal {

void Application::Impl::register_task_commands()
{
    for (const auto method :
        {"research.model.list",
            "research.model.read",
            "research.model.save",
            "research.model.fix",
            "research.model.train",
            "research.model.mapping.save",
            "research.model.mapping.preview",
            "research.model.time.save",
            "research.model.time.train"})
        command(method, [this, method](const json& p) -> PolledTask<Response> {
            if (!research_client)
                throw Error(ErrorCode::unavailable, "select a Research service first");
            const auto client = research_client;
            const std::string_view name(method);
            if (name == "research.model.train") {
                fields(p, {"id", "plan"});
                research::proto::ModelTrainingRequest input;
                *input.mutable_plan() =
                    protocol::encode_strategy_object(p.at("plan"), "model_plan");
                co_await PollFuture{client->submit(text(p, "id"), input)};
                if (research_client != client)
                    throw Error(
                        ErrorCode::conflict,
                        "research owner changed; inspect the original model task");
                co_return snapshot();
            }
            if (name == "research.model.time.train") {
                fields(p, {"id", "plan"});
                research::proto::ModelTimeTrainingRequest input;
                *input.mutable_plan() =
                    protocol::encode_strategy_object(p.at("plan"), "model_time_plan");
                co_await PollFuture{client->submit(text(p, "id"), input)};
                if (research_client != client)
                    throw Error(
                        ErrorCode::conflict,
                        "research owner changed; inspect the original model task");
                co_return snapshot();
            }
            research::proto::ModelRequest request;
            if (name == "research.model.mapping.preview") {
                fields(p, {"mapping", "begin", "limit"});
                const auto& begin = p.at("begin");
                const auto& limit = p.at("limit");
                if (!begin.is_number_integer() || begin < 0 || begin > 200000
                    || !limit.is_number_integer() || limit < 1 || limit > 200)
                    throw std::invalid_argument("diagnostic prediction page outside original rows");
                *request.mutable_preview()->mutable_mapping() =
                    protocol::encode_strategy_object(p.at("mapping"), "model_mapping");
                request.mutable_preview()->set_begin(begin.get<unsigned>());
                request.mutable_preview()->set_limit(limit.get<unsigned>());
            } else if (name == "research.model.read" || name == "research.model.list") {
                if (name == "research.model.read")
                    fields(p, {"kind", "id"});
                else
                    fields(p, {"kind", "after", "limit"});
                const auto kind = text(p, "kind");
                if (kind != "draft" && kind != "plan" && kind != "mapping" && kind != "time_plan")
                    throw std::invalid_argument("invalid model read kind");
                const auto type = kind == "draft" ? research::proto::ModelRead::DRAFT
                    : kind == "plan"              ? research::proto::ModelRead::PLAN
                    : kind == "mapping"           ? research::proto::ModelRead::MAPPING
                                                  : research::proto::ModelRead::TIME_PLAN;
                if (name == "research.model.read") {
                    request.mutable_read()->set_kind(type);
                    request.mutable_read()->set_id(text(p, "id"));
                } else {
                    const auto& limit = p.at("limit");
                    if (!limit.is_number_integer() || limit < 1 || limit > 200)
                        throw std::invalid_argument("task page limit must be 1..200");
                    request.mutable_list()->set_kind(type);
                    request.mutable_list()->set_after(text(p, "after", true));
                    request.mutable_list()->set_limit(limit.get<unsigned>());
                }
            } else {
                auto& mutation = *request.mutable_mutation();
                mutation.set_id(text(p, "id"));
                if (name == "research.model.save") {
                    fields(p, {"id", "draft_id", "expected_digest", "specification"});
                    mutation.mutable_save()->set_draft_id(text(p, "draft_id"));
                    mutation.mutable_save()->set_expected_digest(text(p, "expected_digest", true));
                    *mutation.mutable_save()->mutable_specification() =
                        protocol::encode_model_specification(p.at("specification"));
                } else if (name == "research.model.time.save") {
                    fields(p, {"id", "plan_id", "specification"});
                    mutation.mutable_time_validation()->set_plan_id(text(p, "plan_id"));
                    *mutation.mutable_time_validation()->mutable_specification() =
                        protocol::encode_model_time_specification(p.at("specification"));
                } else if (name == "research.model.mapping.save") {
                    fields(p, {"id", "mapping_id", "specification"});
                    mutation.mutable_mapping()->set_mapping_id(text(p, "mapping_id"));
                    *mutation.mutable_mapping()->mutable_specification() =
                        protocol::encode_model_mapping_specification(p.at("specification"));
                } else {
                    fields(p, {"id", "plan_id", "draft_id", "expected_digest"});
                    mutation.mutable_fix()->set_plan_id(text(p, "plan_id"));
                    mutation.mutable_fix()->set_draft_id(text(p, "draft_id"));
                    mutation.mutable_fix()->set_expected_digest(text(p, "expected_digest"));
                }
                protocol::validate_model_mutation(mutation);
            }
            auto result = co_await PollFuture{client->model(request)};
            if (research_client != client)
                throw Error(
                    ErrorCode::conflict,
                    "research owner changed; inspect the original model operation");
            auto reply = snapshot();
            reply.values["research_model"] = std::move(result);
            co_return reply;
        });
    for (const auto method :
        {"research.batch.list",
            "research.batch.read",
            "research.batch.save",
            "research.batch.start",
            "research.batch.stop"})
        command(method, [this, method](const json& p) -> PolledTask<Response> {
            if (!research_client)
                throw Error(ErrorCode::unavailable, "select a Research service first");
            const auto client = research_client;
            const std::string_view name(method);
            research::proto::BacktestBatchRequest request;
            if (name == "research.batch.list") {
                fields(p, {"after", "limit"});
                const auto& limit = p.at("limit");
                if (!limit.is_number_integer() || limit < 1 || limit > 200)
                    throw std::invalid_argument("batch page limit must be 1..200");
                request.mutable_list()->set_after(text(p, "after", true));
                request.mutable_list()->set_limit(limit.get<unsigned>());
            } else if (name == "research.batch.read") {
                fields(p, {"id"});
                request.mutable_read()->set_id(text(p, "id"));
            } else {
                auto& mutation = *request.mutable_mutation();
                mutation.set_id(text(p, "id"));
                if (name == "research.batch.save") {
                    fields(p, {"id", "definition"});
                    *mutation.mutable_save() =
                        protocol::encode_backtest_batch_draft(p.at("definition"));
                } else if (name == "research.batch.start") {
                    fields(p, {"id", "batch"});
                    *mutation.mutable_start() =
                        protocol::encode_strategy_object(p.at("batch"), "backtest_batch");
                } else {
                    fields(p, {"id", "batch", "reason"});
                    *mutation.mutable_stop()->mutable_batch() =
                        protocol::encode_strategy_object(p.at("batch"), "backtest_batch");
                    mutation.mutable_stop()->set_reason(text(p, "reason"));
                }
                protocol::validate_backtest_batch_mutation(mutation);
            }
            auto result = co_await PollFuture{client->batch(request)};
            if (research_client != client)
                throw Error(
                    ErrorCode::conflict,
                    "research owner changed; inspect the original batch");
            auto reply = snapshot();
            reply.values["research_batch"] = std::move(result);
            co_return reply;
        });

    for (const auto method :
        {"research.batch.assessment.preview",
            "research.batch.assessment.save",
            "research.batch.assessment.read",
            "research.batch.assessment.list"})
        command(method, [this, method](const json& p) -> PolledTask<Response> {
            if (!research_client)
                throw Error(ErrorCode::unavailable, "select a Research service first");
            const auto client = research_client;
            const std::string_view name(method);
            research::proto::BatchAssessmentRequest request;
            if (name == "research.batch.assessment.preview") {
                fields(p, {"batch"});
                *request.mutable_preview() =
                    protocol::encode_strategy_object(p.at("batch"), "backtest_batch");
            } else if (name == "research.batch.assessment.save") {
                fields(p, {"id", "definition"});
                validate_id(text(p, "id"));
                request.mutable_mutation()->set_id(text(p, "id"));
                *request.mutable_mutation()->mutable_save() =
                    protocol::encode_batch_assessment_declaration(p.at("definition"));
            } else if (name == "research.batch.assessment.read") {
                fields(p, {"id"});
                validate_id(text(p, "id"));
                request.set_read(text(p, "id"));
            } else {
                fields(p, {"batch_id", "after", "limit"});
                const auto& limit = p.at("limit");
                if (!limit.is_number_integer() || limit < 1 || limit > 200)
                    throw std::invalid_argument("batch assessment page limit must be 1..200");
                request.mutable_list()->set_batch_id(text(p, "batch_id", true));
                request.mutable_list()->set_after(text(p, "after", true));
                request.mutable_list()->set_limit(limit.get<unsigned>());
            }
            auto result = co_await PollFuture{client->assessment(request)};
            if (research_client != client)
                throw Error(
                    ErrorCode::conflict,
                    "research owner changed; inspect the original batch assessment");
            auto reply = snapshot();
            reply.values["research_assessment"] = std::move(result);
            co_return reply;
        });

    for (const auto method :
        {"research.logic.list",
            "research.logic.read",
            "research.logic.register",
            "research.logic.deprecate",
            "research.logic.examples",
            "research.logic.example.read",
            "research.logic.example.save",
            "research.logic.trial"})
        command(method, [this, method](const json& p) -> PolledTask<Response> {
            if (!research_client)
                throw Error(ErrorCode::unavailable, "select a Research service first");
            const auto client = research_client;
            const std::string_view name(method);
            research::proto::LogicRequest request;
            if (name == "research.logic.trial") {
                fields(p, {"id", "example"});
                research::proto::LogicTrialRequest trial;
                *trial.mutable_example() =
                    protocol::encode_logic_object(p.at("example"), "logic_case");
                co_await PollFuture{client->submit(text(p, "id"), trial)};
                if (research_client != client)
                    throw Error(
                        ErrorCode::conflict,
                        "research owner changed; inspect the original logic trial");
                co_return snapshot();
            }
            if (name == "research.logic.read" || name == "research.logic.example.read") {
                fields(p, {"id"});
                auto& query = name == "research.logic.read" ? *request.mutable_get()
                                                            : *request.mutable_example();
                query.set_id(text(p, "id"));
            } else if (name == "research.logic.list" || name == "research.logic.examples") {
                if (name == "research.logic.list")
                    fields(p, {"after", "limit"});
                else
                    fields(p, {"after", "limit", "definition_id"});
                const auto& limit = p.at("limit");
                if (!limit.is_number_integer() || limit < 1 || limit > 200)
                    throw std::invalid_argument("task page limit must be 1..200");
                auto& page = name == "research.logic.list" ? *request.mutable_list()
                                                           : *request.mutable_examples();
                page.set_after(text(p, "after", true));
                page.set_limit(limit.get<unsigned>());
                if (name == "research.logic.examples")
                    page.set_definition_id(text(p, "definition_id", true));
            } else {
                auto& mutation = *request.mutable_mutation();
                mutation.set_id(text(p, "id"));
                if (name == "research.logic.register") {
                    fields(p, {"id", "definition"});
                    const auto& value = p.at("definition");
                    fields(value, {"id", "name", "purpose", "fast", "slow"});
                    const auto number = [&](const char* key) {
                        const auto& n = value.at(key);
                        if (!n.is_number_integer() || n < 1 || n > 10000)
                            throw std::invalid_argument("logic windows must be 1..10000");
                        return n.get<unsigned>();
                    };
                    auto& definition = *mutation.mutable_registration();
                    definition.set_id(text(value, "id"));
                    definition.set_name(text(value, "name"));
                    definition.set_purpose(text(value, "purpose"));
                    definition.set_fast(number("fast"));
                    definition.set_slow(number("slow"));
                    definition.set_input(research::proto::LogicDefinition::COMPLETED_CLOSE);
                    definition.set_output(research::proto::LogicDefinition::TARGET_SIGN);
                } else if (name == "research.logic.deprecate") {
                    fields(p, {"id", "definition", "reason"});
                    *mutation.mutable_retire()->mutable_definition() =
                        protocol::encode_logic_object(p.at("definition"), "logic_definition");
                    mutation.mutable_retire()->set_reason(text(p, "reason"));
                } else {
                    fields(p, {"id", "example"});
                    const auto& value = p.at("example");
                    fields(
                        value,
                        {"id", "definition", "name", "use_scope", "fictional", "observations"});
                    if (!value.at("fictional").is_boolean() || !value.at("fictional").get<bool>())
                        throw std::invalid_argument("logic examples must be explicitly fictional");
                    auto& example = *mutation.mutable_example();
                    example.set_id(text(value, "id"));
                    *example.mutable_definition() =
                        protocol::encode_logic_object(value.at("definition"), "logic_definition");
                    example.set_name(text(value, "name"));
                    example.set_use_scope(text(value, "use_scope"));
                    example.set_fictional(true);
                    const auto& observations = value.at("observations");
                    if (!observations.is_array() || observations.empty()
                        || observations.size() > 512)
                        throw std::invalid_argument("logic example requires 1..512 observations");
                    const auto time = [](const json& row, const char* key) {
                        const auto raw = text(row, key);
                        std::int64_t ns = 0;
                        const auto parsed =
                            std::from_chars(raw.data(), raw.data() + raw.size(), ns);
                        if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size()
                            || ns <= 0)
                            throw std::invalid_argument(
                                "logic observation requires a positive nanosecond boundary");
                        return ns;
                    };
                    for (const auto& row : observations) {
                        fields(row, {"closed_at_ns", "available_at_ns", "close", "expected"});
                        auto& captured = *example.add_observations();
                        captured.set_closed_at_ns(time(row, "closed_at_ns"));
                        captured.set_available_at_ns(time(row, "available_at_ns"));
                        captured.mutable_close()->set_units(
                            Decimal::parse(text(row, "close")).raw());
                        const auto& expected = row.at("expected");
                        if (text(expected, "state") == "warmup") {
                            fields(expected, {"state"});
                            captured.set_warmup(true);
                        } else {
                            fields(expected, {"state", "value"});
                            const auto& sign = expected.at("value");
                            if (text(expected, "state") != "target_sign"
                                || !sign.is_number_integer() || sign < -1 || sign > 1)
                                throw std::invalid_argument(
                                    "logic expected target sign must be -1, 0 or 1");
                            captured.set_target_sign(sign.get<int>());
                        }
                    }
                }
            }
            auto value = co_await PollFuture{client->logic(request)};
            if (research_client != client)
                throw Error(
                    ErrorCode::conflict,
                    "research owner changed; inspect the original logic definition");
            auto reply = snapshot();
            reply.values["research_logic"] = std::move(value);
            co_return reply;
        });

    for (const auto method :
        {"research.strategy.list",
            "research.strategy.read",
            "research.strategy.freeze",
            "research.strategy.logic.freeze",
            "research.strategy.deprecate",
            "research.strategy.delivery.create",
            "research.strategy.delivery.read",
            "research.strategy.delivery.list"})
        command(method, [this, method](const json& p) -> PolledTask<Response> {
            if (!research_client)
                throw Error(ErrorCode::unavailable, "select a Research service first");
            const auto client = research_client;
            const std::string_view name(method);
            research::proto::StrategyRequest request;
            if (name == "research.strategy.read" || name == "research.strategy.delivery.read") {
                fields(p, {"id"});
                if (name == "research.strategy.read")
                    request.mutable_get()->set_id(text(p, "id"));
                else
                    request.mutable_delivery()->set_id(text(p, "id"));
            } else if (name == "research.strategy.list"
                || name == "research.strategy.delivery.list") {
                if (name == "research.strategy.list")
                    fields(p, {"after", "limit"});
                else
                    fields(p, {"after", "limit", "package_id"});
                const auto& limit = p.at("limit");
                if (!limit.is_number_integer() || limit < 1 || limit > 200)
                    throw std::invalid_argument("task page limit must be 1..200");
                auto& page = name == "research.strategy.list" ? *request.mutable_list()
                                                              : *request.mutable_deliveries();
                page.set_after(text(p, "after", true));
                page.set_limit(limit.get<unsigned>());
                if (name == "research.strategy.delivery.list")
                    page.set_package_id(text(p, "package_id", true));
            } else {
                auto& mutation = *request.mutable_mutation();
                mutation.set_id(text(p, "id"));
                if (name == "research.strategy.freeze") {
                    fields(p, {"id", "registration"});
                    const auto& value = p.at("registration");
                    fields(value, {"id", "name", "purpose", "limitations", "source"});
                    auto& frozen = *mutation.mutable_freeze();
                    frozen.set_id(text(value, "id"));
                    frozen.set_name(text(value, "name"));
                    frozen.set_purpose(text(value, "purpose"));
                    frozen.set_limitations(text(value, "limitations"));
                    *frozen.mutable_source() =
                        protocol::encode_strategy_object(value.at("source"), "backtest_result");
                } else if (name == "research.strategy.logic.freeze") {
                    fields(p, {"id", "registration"});
                    const auto& value = p.at("registration");
                    fields(
                        value,
                        {"id",
                            "name",
                            "purpose",
                            "limitations",
                            "source",
                            "contract",
                            "interval_minutes",
                            "strategy"});
                    auto& frozen = *mutation.mutable_logic();
                    frozen.set_id(text(value, "id"));
                    frozen.set_name(text(value, "name"));
                    frozen.set_purpose(text(value, "purpose"));
                    frozen.set_limitations(text(value, "limitations"));
                    *frozen.mutable_source() =
                        protocol::encode_strategy_object(value.at("source"), "logic_trial_result");
                    *frozen.mutable_contract() = protocol::encode_contract(value.at("contract"));
                    *frozen.mutable_strategy() = protocol::encode_strategy(value.at("strategy"));
                    const auto& interval = value.at("interval_minutes");
                    if (!interval.is_number_integer() || (interval != 0 && interval != 1))
                        throw std::invalid_argument(
                            "logic strategy requires completed one-minute or daily bars");
                    frozen.set_interval_minutes(interval.get<unsigned>());
                } else if (name == "research.strategy.deprecate") {
                    fields(p, {"id", "package", "reason"});
                    *mutation.mutable_retire()->mutable_package() =
                        protocol::encode_strategy_object(p.at("package"), "strategy_package");
                    mutation.mutable_retire()->set_reason(text(p, "reason"));
                } else {
                    fields(p, {"id", "delivery"});
                    const auto& value = p.at("delivery");
                    fields(value, {"id", "package", "target_task_id"});
                    auto& delivery = *mutation.mutable_deliver();
                    delivery.set_id(text(value, "id"));
                    *delivery.mutable_package() =
                        protocol::encode_strategy_object(value.at("package"), "strategy_package");
                    delivery.set_target_task_id(text(value, "target_task_id"));
                }
            }
            auto value = co_await PollFuture{client->strategy(request)};
            if (research_client != client)
                throw Error(
                    ErrorCode::conflict,
                    "research owner changed; inspect the original strategy package");
            auto reply = snapshot();
            reply.values["research_strategy"] = std::move(value);
            co_return reply;
        });

    for (const auto method :
        {"research.variable.check",
            "research.variable.read",
            "research.variable.list",
            "research.variable.register"})
        command(method, [this, method](const json& p) -> PolledTask<Response> {
            if (!research_client)
                throw Error(ErrorCode::unavailable, "select a Research service first");
            const auto client = research_client;
            const std::string_view name(method);
            research::proto::VariableRequest request;
            const auto window = [](const json& value) {
                if (!value.is_number_integer() || value < 0 || value > 10000)
                    throw std::invalid_argument("feature window must be 1..10000");
                return value.get<unsigned>();
            };
            if (name == "research.variable.check") {
                fields(p, {"expression", "window"});
                request.mutable_preview()->set_expression(text(p, "expression", true));
                request.mutable_preview()->set_window(window(p.at("window")));
            } else if (name == "research.variable.read") {
                fields(p, {"id"});
                request.mutable_get()->set_id(text(p, "id"));
            } else if (name == "research.variable.list") {
                fields(p, {"after", "limit"});
                const auto& limit = p.at("limit");
                if (!limit.is_number_integer() || limit < 1 || limit > 200)
                    throw std::invalid_argument("task page limit must be 1..200");
                request.mutable_list()->set_after(text(p, "after", true));
                request.mutable_list()->set_limit(limit.get<unsigned>());
            } else {
                fields(p, {"id", "registration"});
                auto& mutation = *request.mutable_mutation();
                mutation.set_id(text(p, "id"));
                const auto& source = p.at("registration");
                fields(source, {"id", "name", "purpose", "expression", "window"});
                auto& registration = *mutation.mutable_registration();
                registration.set_id(text(source, "id"));
                registration.set_name(text(source, "name"));
                registration.set_purpose(text(source, "purpose"));
                registration.set_expression(text(source, "expression", true));
                registration.set_window(window(source.at("window")));
            }
            auto value = co_await PollFuture{client->variable(request)};
            if (research_client != client)
                throw Error(
                    ErrorCode::conflict,
                    "research owner changed; inspect the original variable");
            auto reply = snapshot();
            reply.values["research_variable"] = std::move(value);
            co_return reply;
        });
    for (const auto method :
        {"research.project.list",
            "research.project.read",
            "research.project.save",
            "research.plan.list",
            "research.plan.read",
            "research.plan.fix",
            "research.project.tasks",
            "research.conclusion.read",
            "research.conclusion.list",
            "research.conclusion.append"})
        command(method, [this, method](const json& p) -> PolledTask<Response> {
            if (!research_client)
                throw Error(ErrorCode::unavailable, "select a Research service first");
            const auto client = research_client;
            const std::string_view name(method);
            const auto limit = [&] {
                const auto& value = p.at("limit");
                if (!value.is_number_integer() || value < 1 || value > 200)
                    throw std::invalid_argument("task page limit must be 1..200");
                return value.get<unsigned>();
            };
            research::proto::ProjectRequest request;
            research::proto::ProjectTaskQuery tasks;
            if (name == "research.conclusion.read") {
                fields(p, {"id"});
                request.mutable_conclusion()->set_id(text(p, "id"));
            } else if (name == "research.conclusion.list") {
                fields(p, {"project_id", "after", "limit"});
                auto& query = *request.mutable_conclusions();
                query.set_project_id(text(p, "project_id", true));
                query.set_after(text(p, "after", true));
                query.set_limit(limit());
            } else if (name == "research.conclusion.append") {
                fields(p, {"id", "declaration"});
                auto& mutation = *request.mutable_mutation();
                mutation.set_id(text(p, "id"));
                *mutation.mutable_conclude() =
                    protocol::encode_project_conclusion(p.at("declaration"));
            } else if (name == "research.project.list") {
                fields(p, {"after", "limit"});
                auto& query = *request.mutable_drafts();
                query.set_after(text(p, "after", true));
                query.set_limit(limit());
            } else if (name == "research.plan.list") {
                fields(p, {"project_id", "after", "limit"});
                auto& query = *request.mutable_plans();
                query.set_project_id(text(p, "project_id", true));
                query.set_after(text(p, "after", true));
                query.set_limit(limit());
            } else if (name == "research.project.read") {
                fields(p, {"id"});
                request.mutable_draft()->set_id(text(p, "id"));
            } else if (name == "research.plan.read") {
                fields(p, {"id"});
                request.mutable_plan()->set_id(text(p, "id"));
            } else if (name == "research.project.save") {
                fields(p, {"id", "project_id", "expected_digest", "definition"});
                auto& command = *request.mutable_mutation();
                command.set_id(text(p, "id"));
                auto& save = *command.mutable_save();
                save.set_project_id(text(p, "project_id"));
                save.set_expected_digest(text(p, "expected_digest", true));
                *save.mutable_definition() =
                    protocol::encode_project_definition(p.at("definition"));
            } else if (name == "research.plan.fix") {
                fields(
                    p,
                    {"id",
                        "project_id",
                        "plan_id",
                        "expected_draft_digest",
                        "parent_plan_id",
                        "revision_reason"});
                auto& command = *request.mutable_mutation();
                command.set_id(text(p, "id"));
                auto& fix = *command.mutable_fix();
                fix.set_project_id(text(p, "project_id"));
                fix.set_plan_id(text(p, "plan_id"));
                fix.set_expected_draft_digest(text(p, "expected_draft_digest"));
                fix.set_parent_plan_id(text(p, "parent_plan_id", true));
                fix.set_revision_reason(text(p, "revision_reason", true));
            } else {
                fields(p, {"project_id", "before_sequence", "limit"});
                const auto& before = p.at("before_sequence");
                if (!before.is_number_integer() || before < 0 || before > 4294967295ULL)
                    throw std::invalid_argument("invalid task page cursor");
                tasks.set_project_id(text(p, "project_id"));
                tasks.set_limit(limit());
                tasks.set_before_sequence(before.get<unsigned>());
            }
            auto value = co_await PollFuture{
                name == "research.project.tasks" ? client->project_tasks(tasks)
                                                 : client->project(request)
            };
            if (research_client != client)
                throw Error(
                    ErrorCode::conflict,
                    "research owner changed; inspect the original project");
            auto reply = snapshot();
            reply.values[name == "research.project.tasks" ? "project_tasks" : "research_project"] =
                std::move(value);
            co_return reply;
        });
    command("research.sample.submit", [this](const json& p) -> PolledTask<Response> {
        auto required = p;
        required.erase("research_plan");
        fields(
            required,
            {"id", "owner", "data_owner", "selection", "use_scope", "mode", "evaluation"});
        validate_id(text(p, "id"));
        if (task_owner_is_data(p))
            throw std::invalid_argument("invalid research sample owner");
        const auto client = research_client;
        const auto& owner = p.at("data_owner");
        fields(owner, {"node_id", "service_id"});
        data::proto::DataOwner source;
        source.set_node_id(text(owner, "node_id"));
        source.set_service_id(text(owner, "service_id"));
        validate_id(source.node_id());
        validate_id(source.service_id());
        const auto input = protocol::encode_sample_request(
            {{"selection", p.at("selection")},
                {"use_scope", p.at("use_scope")},
                {"mode", p.at("mode")},
                {"evaluation", p.at("evaluation")}});
        research::proto::ProjectBinding plan;
        if (p.contains("research_plan"))
            plan = protocol::encode_project_binding(p.at("research_plan"));
        co_await PollFuture{client->submit(text(p, "id"), input, source, plan)};
        if (research_client != client)
            throw Error(
                ErrorCode::conflict,
                "data/research service selection changed during operation; inspect the original services before retrying");
        task_result = nullptr;
        co_return snapshot();
    });
    command("evaluation.submit", [this](const json& p) -> PolledTask<Response> {
        auto required = p;
        required.erase("include_ledger");
        required.erase("include_contributions");
        fields(
            required,
            {"id",
                "owner",
                "source",
                "question",
                "scope",
                "method",
                "include_daily_drawdown",
                "include_exposure",
                "static_stress"});
        validate_id(text(p, "id"));
        if (task_owner_is_data(p))
            throw std::invalid_argument("evaluation source must belong to this Research service");
        const auto client = research_client;
        auto parameters = p;
        parameters.erase("id");
        parameters.erase("owner");
        const auto input = protocol::encode_evaluation_request(parameters);
        const auto endpoint = client->endpoint();
        if (input.source().owner().node_id() != endpoint.owner_node_id
            || input.source().owner().service_id() != endpoint.session)
            throw Error(
                ErrorCode::conflict,
                "evaluation source must belong to this Research service");
        co_await PollFuture{client->submit(text(p, "id"), input)};
        if (research_client != client)
            throw Error(
                ErrorCode::conflict,
                "data/research service selection changed during operation; inspect the original services before retrying");
        task_result = nullptr;
        co_return snapshot();
    });
    command("statistics.submit", [this](const json& p) -> PolledTask<Response> {
        auto required = p;
        required.erase("research_plan");
        required.erase("grouping");
        fields(
            required,
            {"id",
                "owner",
                "source",
                "use_scope",
                "column",
                "role",
                "begin_index",
                "end_index",
                "bins"});
        validate_id(text(p, "id"));
        if (task_owner_is_data(p))
            throw std::invalid_argument("statistics source must belong to this Research service");
        const auto client = research_client;
        auto parameters = p;
        parameters.erase("id");
        parameters.erase("owner");
        parameters.erase("research_plan");
        const auto input = protocol::encode_statistics_request(parameters);
        const auto endpoint = client->endpoint();
        if (input.source().owner().node_id() != endpoint.owner_node_id
            || input.source().owner().service_id() != endpoint.session)
            throw Error(
                ErrorCode::conflict,
                "statistics source must belong to this Research service");
        research::proto::ProjectBinding plan;
        if (p.contains("research_plan"))
            plan = protocol::encode_project_binding(p.at("research_plan"));
        co_await PollFuture{client->submit(text(p, "id"), input, plan)};
        if (research_client != client)
            throw Error(
                ErrorCode::conflict,
                "data/research service selection changed during operation; inspect the original services before retrying");
        task_result = nullptr;
        co_return snapshot();
    });
    command("notice.review.submit", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "owner", "source", "as_of_ms", "use_scope"});
        validate_id(text(p, "id"));
        if (task_owner_is_data(p))
            throw std::invalid_argument("notice review requires a Research owner");
        const auto client = research_client;
        const auto& source = p.at("source");
        fields(source, {"node", "service", "domain", "kind", "id", "version"});
        research::proto::NoticeReviewRequest input;
        auto& ref = *input.mutable_source();
        ref.mutable_owner()->set_node_id(text(source, "node"));
        ref.mutable_owner()->set_service_id(text(source, "service"));
        ref.mutable_owner()->set_domain(text(source, "domain"));
        ref.set_kind(text(source, "kind"));
        ref.set_id(text(source, "id"));
        ref.set_version(text(source, "version"));
        const auto raw = text(p, "as_of_ms");
        std::int64_t cutoff = 0;
        const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), cutoff);
        if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size() || cutoff <= 0)
            throw std::invalid_argument(
                "notice review cutoff must be a positive UTC millisecond timestamp");
        input.set_as_of_ms(cutoff);
        input.set_use_scope(text(p, "use_scope"));
        protocol::validate_notice_review_request(input);
        co_await PollFuture{client->submit(text(p, "id"), input)};
        if (research_client != client)
            throw Error(
                ErrorCode::conflict,
                "research owner changed; inspect the original notice review");
        task_result = nullptr;
        co_return snapshot();
    });
    command("event.set.review.submit", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "owner", "source", "use_scope"});
        validate_id(text(p, "id"));
        if (task_owner_is_data(p))
            throw std::invalid_argument("event set review requires a Research owner");
        const auto client = research_client;
        const auto& source = p.at("source");
        fields(source, {"node", "service", "domain", "kind", "id", "version"});
        research::proto::EventSetReviewRequest input;
        auto& ref = *input.mutable_source();
        ref.mutable_owner()->set_node_id(text(source, "node"));
        ref.mutable_owner()->set_service_id(text(source, "service"));
        ref.mutable_owner()->set_domain(text(source, "domain"));
        ref.set_kind(text(source, "kind"));
        ref.set_id(text(source, "id"));
        ref.set_version(text(source, "version"));
        input.set_use_scope(text(p, "use_scope"));
        protocol::validate_event_set_review_request(input);
        co_await PollFuture{client->submit(text(p, "id"), input)};
        if (research_client != client)
            throw Error(
                ErrorCode::conflict,
                "research owner changed; inspect the original event set review");
        task_result = nullptr;
        co_return snapshot();
    });
    command("notice.source.inspect", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "owner"});
        validate_id(text(p, "id"));
        if (task_owner_is_data(p))
            throw std::invalid_argument("notice review requires a Research owner");
        const auto client = research_client;
        auto observation = co_await PollFuture{client->notice_source_changes(text(p, "id"))};
        if (research_client != client)
            throw Error(
                ErrorCode::conflict,
                "research owner changed; inspect the original notice review");
        auto result = snapshot();
        result.values["notice_source_observation"] = std::move(observation);
        co_return result;
    });
    command("notice.custody.reconcile", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "owner"});
        validate_id(text(p, "id"));
        if (task_owner_is_data(p))
            throw std::invalid_argument("notice review requires a Research owner");
        const auto client = research_client;
        if (!client)
            throw Error(ErrorCode::unavailable, "select a Research service first");
        auto value = co_await PollFuture{client->reconcile_notice_custody(text(p, "id"))};
        if (research_client != client)
            throw Error(
                ErrorCode::conflict,
                "research owner changed; inspect the original notice review");
        auto result = snapshot();
        notice_custody = std::make_shared<const json>(value);
        result.values["notice_custody"] = std::move(value);
        co_return result;
    });
    command("account.review.submit", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "owner", "source", "account_id", "environment"});
        validate_id(text(p, "id"));
        if (task_owner_is_data(p))
            throw std::invalid_argument("account review requires a Research owner");
        const auto client = research_client;
        const auto& source = p.at("source");
        fields(source, {"node", "service", "domain", "kind", "id", "version"});
        research::proto::AccountReviewRequest input;
        auto& ref = *input.mutable_source();
        ref.mutable_owner()->set_node_id(text(source, "node"));
        ref.mutable_owner()->set_service_id(text(source, "service"));
        ref.mutable_owner()->set_domain(text(source, "domain"));
        ref.set_kind(text(source, "kind"));
        ref.set_id(text(source, "id"));
        ref.set_version(text(source, "version"));
        input.set_account_id(text(p, "account_id"));
        const auto environment = text(p, "environment");
        if (environment != "real" && environment != "simulation")
            throw std::invalid_argument("account review requires an explicit trading environment");
        input.set_environment(
            environment == "real" ? protocol::proto::REAL : protocol::proto::BROKER_SIMULATION);
        protocol::validate_account_review_request(input);
        co_await PollFuture{client->submit(text(p, "id"), input)};
        if (research_client != client)
            throw Error(
                ErrorCode::conflict,
                "research owner changed; inspect the original account review");
        co_return snapshot();
    });
    command("task.page", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"before_sequence", "owner"});
        const bool data = task_owner_is_data(p);
        const auto& before = p.at("before_sequence");
        if (!before.is_number_integer() || before < 0 || before > 4294967295ULL)
            throw std::invalid_argument("invalid task page cursor");
        const auto generation = data_task_generation;
        co_await PollFuture{
            data ? data_client->page(before.get<unsigned>())
                 : research_client->page(before.get<unsigned>())
        };
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "task owner selection changed; inspect the original service");
        co_return snapshot();
    });
    command("task.action", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "action", "owner"});
        const bool data = task_owner_is_data(p);
        co_await PollFuture{
            data ? data_client->action(text(p, "id"), text(p, "action"))
                 : research_client->action(text(p, "id"), text(p, "action"))
        };
        co_return snapshot();
    });
    for (const auto method : {"task.retirement.preview", "task.retirement.execute"})
        command(method, [this, method = std::string(method)](const json& p) -> PolledTask<Response> {
            fields(p, {"id", "owner"});
            const bool data = task_owner_is_data(p);
            const auto client = research_client;
            const auto generation = data_task_generation;
            auto value = co_await PollFuture{
                data ? data_client->retirement(text(p, "id"), method == "task.retirement.execute")
                     : client->retirement(text(p, "id"), method == "task.retirement.execute")
            };
            if (generation != data_task_generation)
                throw Error(
                    ErrorCode::conflict,
                    "task owner selection changed; inspect the original service before retrying retirement");
            auto reply = snapshot();
            reply.values["task_retirement"] = std::move(value);
            co_return reply;
        });
    command("task.comparison.export", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"expected", "matrix", "path"});
        if (!research_client)
            throw std::invalid_argument("research service is not connected");
        const auto& expected = p.at("expected");
        if (!expected.is_array() || expected.size() < 2 || expected.size() > 3)
            throw std::invalid_argument("invalid research comparison evidence");
        const auto client = research_client;
        const auto generation = data_task_generation;
        const auto instance = client->owner_view().connection.at("service").get<std::string>();
        std::set<std::string> ids;
        std::vector<json> sources;
        for (const auto& reference : expected) {
            fields(reference, {"id", "result_digest", "metadata_revision", "input_sha256"});
            const auto id = text(reference, "id");
            if (!ids.insert(id).second)
                throw std::invalid_argument("invalid research comparison evidence");
            auto source = co_await PollFuture{client->result(id)};
            if (generation != data_task_generation)
                throw Error(
                    ErrorCode::conflict,
                    "data/research service selection changed during operation; " "inspect the original services before retrying");
            if (source.at("task").at("result_digest") != reference.at("result_digest")
                || source.at("task").at("metadata").at("revision")
                    != reference.at("metadata_revision")
                || source.value("input_sha256", json(nullptr)) != reference.at("input_sha256"))
                throw Error(
                    ErrorCode::conflict,
                    "comparison evidence changed; reload comparison before exporting");
            sources.push_back(std::move(source));
        }
        auto exported = co_await service_io.admin<json>([&] {
            const auto content = comparison_evidence_document(sources, instance, p.at("matrix"));
            json receipt = {
                {"name", "research-comparison.evidence.json"},
                {"bytes", content.size()}
            };
            const auto path = p.at("path").get<std::string>();
            if (path.empty())
                receipt["content"] = content;
            else {
                save_research_evidence(
                    std::filesystem::path(std::u8string(path.begin(), path.end())),
                    content);
                receipt["path"] = path;
            }
            return receipt;
        });
        auto reply = snapshot();
        reply.values["research_export"] = std::move(exported);
        co_return reply;
    });
    command("research.batch.assessment.export", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"object", "format", "path"});
        if (!research_client)
            throw Error(ErrorCode::unavailable, "select a Research service first");
        const auto object =
            protocol::encode_strategy_object(p.at("object"), "backtest_batch_assessment");
        const auto client = research_client;
        const auto endpoint = client->endpoint();
        if (object.owner().node_id() != endpoint.owner_node_id
            || object.owner().service_id() != endpoint.session)
            throw Error(ErrorCode::conflict, "resource belongs to another selected owner");
        const auto format = text(p, "format");
        if (format != "html" && format != "csv")
            throw std::invalid_argument("fixed batch assessment export requires HTML or CSV");
        research::proto::BatchAssessmentRequest read;
        read.set_read(object.id());
        auto response = co_await PollFuture{client->assessment(read)};
        auto fixed = response.at("assessment");
        if (fixed.is_null() || fixed.at("object") != p.at("object"))
            throw Error(
                ErrorCode::conflict,
                "fixed batch assessment changed; reload its saved record");
        auto source = co_await PollFuture{client->result(
            fixed.at("snapshot").at("source").at("id").get<std::string>())};
        if (research_client != client)
            throw Error(
                ErrorCode::conflict,
                "research owner changed; inspect the original batch assessment");
        const auto path = text(p, "path", true);
        auto exported = co_await service_io.admin<json>(
            [fixed = std::move(fixed), source = std::move(source), format, path] {
                const auto content = batch_assessment_document(fixed, source, format);
                json receipt = {
                    {"name",
                        fixed.at("object").at("id").get<std::string>() + ".assessment." + format},
                    {"bytes", content.size()},
                    {"source_version", fixed.at("object").at("version")},
                    {"conclusion_version", ""}
                };
                if (path.empty())
                    receipt["content"] = content;
                else {
                    save_research_evidence(
                        std::filesystem::path(std::u8string(path.begin(), path.end())),
                        content);
                    receipt["path"] = path;
                }
                return receipt;
            });
        auto reply = snapshot();
        reply.values["research_export"] = std::move(exported);
        co_return reply;
    });
    command("evaluation.report.export", [this](const json& p) -> PolledTask<Response> {
        fields(
            p,
            {"id", "owner", "version", "format", "conclusion_id", "conclusion_version", "path"});
        if (task_owner_is_data(p))
            throw std::invalid_argument("fixed evaluation report requires its Research owner");
        const auto client = research_client;
        const auto format = text(p, "format");
        if (format != "html" && format != "csv")
            throw std::invalid_argument("fixed evaluation report export requires HTML or CSV");
        auto source = co_await PollFuture{client->result(text(p, "id"))};
        if (source.at("kind") != "performance_evaluation"
            || source.at("task").at("result_digest") != text(p, "version"))
            throw Error(
                ErrorCode::conflict,
                "fixed evaluation changed; reload its published report");
        const auto conclusion_id = text(p, "conclusion_id", true);
        const auto conclusion_version = text(p, "conclusion_version", true);
        json human = nullptr;
        if (!conclusion_id.empty()) {
            research::proto::ProjectRequest read;
            read.mutable_conclusion()->set_id(conclusion_id);
            const auto response = co_await PollFuture{client->project(read)};
            human = response.at("conclusion");
            if (human.at("digest") != conclusion_version)
                throw Error(
                    ErrorCode::conflict,
                    "human interpretation changed; reload its fixed record");
        } else if (!conclusion_version.empty())
            throw std::invalid_argument("human interpretation identity is incomplete");
        if (research_client != client)
            throw Error(
                ErrorCode::conflict,
                "data/research service selection changed during operation; inspect the original services before retrying");
        const auto path = text(p, "path", true);
        auto exported = co_await service_io.admin<json>(
            [source = std::move(source), human = std::move(human), format, path] {
                const auto content = evaluation_report_document(source, human, format);
                json receipt = {
                    {"name", source.at("id").get<std::string>() + ".evaluation." + format},
                    {"bytes", content.size()},
                    {"source_version", source.at("task").at("result_digest")},
                    {"conclusion_version", human.is_null() ? json("") : human.at("digest")}
                };
                if (path.empty())
                    receipt["content"] = content;
                else {
                    save_research_evidence(
                        std::filesystem::path(std::u8string(path.begin(), path.end())),
                        content);
                    receipt["path"] = path;
                }
                return receipt;
            });
        auto reply = snapshot();
        reply.values["research_export"] = std::move(exported);
        co_return reply;
    });
    command("task.evidence.export", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "path"});
        if (!research_client)
            throw std::invalid_argument("research service is not connected");
        const auto client = research_client;
        const auto generation = data_task_generation;
        const auto instance = client->owner_view().connection.at("service").get<std::string>();
        auto source = co_await PollFuture{client->result(text(p, "id"))};
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/research service selection changed during operation; " "inspect the original services before retrying");
        const auto path = p.at("path").get<std::string>();
        auto exported =
            co_await service_io.admin<json>([source = std::move(source), path, instance] {
                const auto content = research_evidence_document(source, instance);
                json receipt = {
                    {"name", source.at("id").get<std::string>() + ".evidence.json"},
                    {"bytes", content.size()}
                };
                if (path.empty())
                    receipt["content"] = content;
                else {
                    save_research_evidence(
                        std::filesystem::path(std::u8string(path.begin(), path.end())),
                        content);
                    receipt["path"] = path;
                }
                return receipt;
            });
        auto reply = snapshot();
        reply.values["research_export"] = std::move(exported);
        co_return reply;
    });
    command("task.metadata.read", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "owner"});
        const bool data = task_owner_is_data(p);
        const auto generation = data_task_generation;
        auto task = co_await PollFuture{
            data ? data_client->metadata(text(p, "id")) : research_client->metadata(text(p, "id"))
        };
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "task owner selection changed; inspect the original service");
        auto reply = snapshot();
        reply.values["task_description"] = std::move(task);
        co_return reply;
    });
    command("task.metadata.edit", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "expected_revision", "name", "note", "owner"});
        const bool data = task_owner_is_data(p);
        const auto& revision = p.at("expected_revision");
        if (!revision.is_number_integer() || revision < 0 || revision > 4294967295ULL
            || !p.at("name").is_string() || !p.at("note").is_string())
            throw std::invalid_argument("invalid experiment metadata");
        task::proto::TaskMetadataEdit edit;
        edit.set_id(text(p, "id"));
        edit.set_expected_revision(revision.get<unsigned>());
        edit.set_name(p.at("name").get<std::string>());
        edit.set_note(p.at("note").get<std::string>());
        co_await PollFuture{
            data ? data_client->edit_metadata(edit) : research_client->edit_metadata(edit)
        };
        co_return snapshot();
    });
    command("task.portfolio.analyze", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"expected", "training_end"});
        const auto& expected = p.at("expected");
        if (!expected.is_array() || expected.size() < 2 || expected.size() > 3)
            throw Error(
                ErrorCode::invalid_request,
                "Portfolio analysis requires 2 or 3 distinct backtests");
        const auto training_end = text(p, "training_end");
        parse_trading_date(training_end);
        if (!research_client)
            throw std::invalid_argument("research service is not connected");
        const auto client = research_client;
        const auto generation = data_task_generation;
        std::set<std::string> ids;
        std::vector<json> sources;
        for (const auto& reference : expected) {
            fields(reference, {"id", "result_digest"});
            const auto id = text(reference, "id");
            validate_id(id);
            if (!ids.insert(id).second)
                throw Error(
                    ErrorCode::invalid_request,
                    "Portfolio analysis requires 2 or 3 distinct backtests");
            auto source = co_await PollFuture{client->result(id)};
            if (generation != data_task_generation)
                throw Error(
                    ErrorCode::conflict,
                    "data/research service selection changed during operation; inspect the original services before retrying");
            if (source.at("task").at("result_digest") != text(reference, "result_digest"))
                throw Error(
                    ErrorCode::conflict,
                    "backtest result changed; reopen the result before analysis");
            sources.push_back(std::move(source));
        }
        auto report = co_await service_io.admin<json>([sources = std::move(sources), training_end] {
            return portfolio_report(
                sources,
                training_end,
                environment_path("ASTERION_ANALYTICS_EXECUTABLE")
                    .value_or(std::filesystem::path{}));
        });
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/research service selection changed during operation; inspect the original services before retrying");
        auto reply = snapshot();
        reply.values["portfolio_report"] = std::move(report);
        co_return reply;
    });
    command("task.quantstats.analyze", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "result_digest"});
        if (!research_client)
            throw std::invalid_argument("research service is not connected");
        validate_id(text(p, "id"));
        const auto client = research_client;
        const auto generation = data_task_generation;
        auto source = co_await PollFuture{client->result(text(p, "id"))};
        if (source.at("task").at("result_digest") != text(p, "result_digest"))
            throw Error(
                ErrorCode::conflict,
                "backtest result changed; reopen the result before analysis");
        auto report = co_await service_io.admin<json>([source = std::move(source)] {
            return quantstats_report(
                source,
                environment_path("ASTERION_ANALYTICS_EXECUTABLE")
                    .value_or(std::filesystem::path{}));
        });
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/research service selection changed during operation; inspect the original services before retrying");
        auto reply = snapshot();
        reply.values["quantstats_report"] = std::move(report);
        co_return reply;
    });
    for (const auto method : {"task.result.read", "task.result"})
        command(
            method,
            [this, select = std::string(method) == "task.result"](
                const json& p) -> PolledTask<Response> {
                fields(p, {"id", "owner"});
                const bool data = task_owner_is_data(p);
                const auto generation = data_task_generation;
                auto evidence = co_await PollFuture{
                    data ? data_client->result(text(p, "id"))
                         : research_client->result(text(p, "id"))
                };
                if (generation != data_task_generation)
                    throw Error(
                        ErrorCode::conflict,
                        "task owner selection changed; inspect the original service");
                if (select) {
                    task_result = std::make_shared<const json>(std::move(evidence));
                    co_return snapshot();
                }
                auto reply = snapshot();
                reply.values["task_result"] = std::move(evidence);
                co_return reply;
            });
}

} // namespace asterion::terminal
