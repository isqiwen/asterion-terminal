#include "research_tasks.hpp"
#include <asterion/futures/order_limits.hpp>
#include "task_payload.hpp"
#include <asterion/base/error.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/protocol/backtest.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/research_evaluation.hpp>
#include <asterion/protocol/research_logic.hpp>
#include <asterion/protocol/research_sample.hpp>
#include <asterion/protocol/research_statistics.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/research_variable.hpp>
#include <asterion/protocol/resource.hpp>
#include <asterion/protocol/task_execution.hpp>
#include <algorithm>
#include <array>

namespace asterion::research {
namespace wire = task::proto;
namespace fs = std::filesystem;
using tasks::payload::validate_digest;

namespace {

// Kind, resource, manifest, definition, result, finish; calculation, publishes,
// execution carries input, references history.
constexpr std::array<tasks::KindSpec, 6> research_kinds{{
    {wire::BACKTEST,
        "backtest",
        "backtest.task",
        "input",
        "backtest",
        "result",
        true,
        false,
        true,
        true},
    {wire::FACTOR, "factor", "factor.task", "factor", "factor", "factor", true, false, true, true},
    {wire::STATISTICS,
        "statistics",
        "statistics.task",
        "statistics",
        "statistics",
        "statistics",
        true,
        false,
        false,
        false},
    {wire::PERFORMANCE_EVALUATION,
        "performance_evaluation",
        "performance_evaluation.task",
        "evaluation",
        "evaluation",
        "evaluation",
        true,
        false,
        false,
        false},
    {wire::SAMPLE, "sample", "sample.task", "sample", "sample", "sample", true, false, false, true},
    {wire::LOGIC_TRIAL,
        "logic_trial",
        "logic_trial.task",
        "logic_trial",
        "logic_trial",
        "logic_trial",
        true,
        false,
        false,
        false},
}};

// Program files live beside the tasks.
constexpr std::array<std::string_view, 1> research_directories{"programs"};

// How a list names a dominant series: the product by the first and last month
// it is read from.
std::string series_name(const data::proto::BarDataset& first, const data::proto::BarDataset& last)
{
    const auto& contract = first.contract();
    const auto months = contract.symbol() == last.contract().symbol()
        ? contract.symbol()
        : contract.symbol() + "-" + last.contract().symbol();
    return contract.venue() + "/" + contract.product() + " (" + months + ")";
}

// What a list says a dataset was read from: its own revision, or for daily
// bars of the daily table the snapshot, which datasets read together name
// once.
void name_source(std::string& sources, const data::proto::BarDataset& dataset)
{
    const auto& name =
        dataset.source_dataset_ids().empty() ? dataset.daily().snapshot() : dataset.revision();
    if (!sources.ends_with(name))
        sources += (sources.empty() ? "" : " + ") + name;
}

void describe_factor(wire::Task& task)
{
    protocol::validate_factor_input(task.factor());
    if (task.factor().has_variable_definition())
        *task.mutable_variable_definition() =
            protocol::variable_object(task.factor().variable_definition());
    else if (task.has_variable_definition())
        throw std::invalid_argument("research variable definition missing from fixed input");
    std::string instruments, sources;
    for (const auto& series : task.factor().series()) {
        const auto separator = instruments.empty() ? "" : " + ";
        if (series.has_bars()) {
            const auto& c = series.bars().contract();
            instruments += separator + c.venue() + "/" + c.symbol();
            name_source(sources, series.bars());
        } else if (series.has_dominant()) {
            const auto& months = series.dominant().months();
            instruments += separator + series_name(*months.begin(), *months.rbegin());
            for (const auto& month : months)
                name_source(sources, month);
        } else {
            // Every instrument of one snapshot names it once.
            instruments += separator + series.table().instrument();
            if (!sources.ends_with(series.table().snapshot()))
                sources += separator + series.table().snapshot();
        }
    }
    task.set_instrument(instruments);
    task.set_source_name(sources);
    task.set_total(static_cast<unsigned>(protocol::factor_work_units(task.factor())));
    task.clear_trading_day();
}

void describe_backtest(wire::Task& task)
{
    (void)protocol::decode_backtest(task.input(), protocol::DatasetView::metadata);
    const auto& paper = task.input().paper();
    const auto dataset = [&](unsigned contract) -> const data::proto::BarDataset& {
        return paper.contracts(static_cast<int>(contract)).dataset();
    };
    // The series each contract is a month of, if any.
    std::vector<const data::proto::DominantSchedule*> series(
        static_cast<std::size_t>(paper.contracts_size()));
    for (const auto& schedule : task.input().series())
        for (const auto& roll : schedule.rolls())
            series[roll.contract()] = &schedule;
    // Contracts outside a series share their trading days; a series runs from
    // its first month's first day to its last month's last.
    std::string instruments, sources;
    auto first = dataset(0).days(0).trading_day(), last = first;
    for (unsigned c = 0; c < series.size(); ++c) {
        first = std::min(first, dataset(c).days(0).trading_day());
        last = std::max(last, dataset(c).days().rbegin()->trading_day());
        const auto separator = instruments.empty() ? "" : " + ";
        if (!series[c]) {
            const auto& contract = dataset(c).contract();
            instruments += separator + contract.venue() + "/" + contract.symbol();
            name_source(sources, dataset(c));
        } else if (series[c]->rolls(0).contract() == c) {
            // Named once, where its first month stands.
            instruments += separator
                + series_name(dataset(c), dataset(series[c]->rolls().rbegin()->contract()));
            for (const auto& roll : series[c]->rolls())
                name_source(sources, dataset(roll.contract()));
        }
    }
    task.set_instrument(instruments);
    task.set_source_name(sources);
    task.set_trading_day(first == last ? first : first + " / " + last);
    task.set_total(static_cast<unsigned>(protocol::backtest_work_units(task.input())));
}

void require_program(const wire::Task& task, const char* missing)
{
    if (task.program_artifact().empty())
        throw Error(ErrorCode::conflict, missing);
    validate_digest(task.program_artifact());
}

// A derivation relates the new task to a succeeded original of this service.
void admit_derivation(tasks::Admission& admission)
{
    auto& task = admission.task;
    const auto& source = task.metadata().derivation();
    const auto original_entry = admission.records.find(source.source_id());
    const auto& original = original_entry.task();
    if (admission.records.retired(source.source_id()))
        throw Error(ErrorCode::conflict, "retired research source cannot accept new derivations");
    const bool statistics = task.kind() == wire::STATISTICS;
    const bool evaluation = task.kind() == wire::PERFORMANCE_EVALUATION;
    if (source.source_id() == task.id() || original.state() != wire::SUCCEEDED
        || (statistics ? !((original.kind() == wire::FACTOR
                               && task.statistics().source().result().kind() == "factor_result")
                             || (original.kind() == wire::SAMPLE
                                 && task.statistics().source().result().kind() == "sample_result"))
                : evaluation ? original.kind() != wire::BACKTEST
                             : original.kind() != task.kind())
        || original.result_digest() != source.source_result_digest())
        throw std::invalid_argument("invalid experiment derivation");
    if (evaluation) {
        const auto& fixed = task.evaluation();
        const auto& identity = fixed.request().source();
        const auto& proof = fixed.source();
        if (source.data_choice() != "fixed" || source.source_id() != identity.id()
            || source.source_result_digest() != identity.version()
            || proof.input_sha256() != original_entry.input_digest()
            || proof.input_bytes() != original_entry.input_bytes()
            || proof.attempt() != original.attempt()
            || proof.program_artifact() != original.program_artifact())
            throw Error(
                ErrorCode::conflict,
                "evaluation source evidence differs from its captured identity");
        // Account bytes are the parent evidence, not reusable evaluation input.
    } else if (statistics) {
        const auto& fixed = task.statistics().source();
        if (source.data_choice() != "fixed" || source.source_id() != fixed.result().id()
            || source.source_result_digest() != fixed.result().version()
            || fixed.input_sha256() != original_entry.input_digest()
            || fixed.input_bytes() != original_entry.input_bytes()
            || fixed.attempt() != original.attempt()
            || fixed.program_artifact() != original.program_artifact())
            throw Error(
                ErrorCode::conflict,
                "statistics source evidence does not match its captured identity");
        // The parent is factor evidence, not reusable statistics input bytes.
    } else
        admission.reusable = original_entry;
}

class ResearchTasks final : public tasks::TaskDomain
{
public:
    std::string_view name() const override
    {
        return "research";
    }

    std::span<const tasks::KindSpec> kinds() const override
    {
        return research_kinds;
    }

    std::span<const std::string_view> directories() const override
    {
        return research_directories;
    }

    void describe(wire::Task& task) const override
    {
        switch (task.kind()) {
            case wire::LOGIC_TRIAL:
                protocol::validate_logic_trial_input(task.logic_trial());
                task.set_source_name("fictional_logic_examples");
                task.set_instrument(task.logic_trial().definition().name());
                task.set_total(
                    static_cast<unsigned>(task.logic_trial().example().observations_size()));
                break;
            case wire::SAMPLE:
                protocol::validate_sample_input(task.sample());
                task.set_source_name(task.sample().dataset().source());
                task.set_instrument(task.sample().request().selection().contract());
                task.set_total(2 * static_cast<unsigned>(task.sample().dataset().bars_size()));
                break;
            case wire::PERFORMANCE_EVALUATION:
                protocol::validate_evaluation_input(task.evaluation());
                task.set_source_name(task.evaluation().request().source().owner().service_id());
                task.set_instrument(task.evaluation().request().source().id());
                task.set_total(
                    task.evaluation().source().end_settlement()
                    - task.evaluation().source().first_settlement());
                break;
            case wire::STATISTICS:
                protocol::validate_statistics_input(task.statistics());
                task.set_source_name(task.statistics().request().source().owner().service_id());
                task.set_instrument(task.statistics().request().source().id());
                task.set_total(
                    (task.statistics().request().grouping()
                                == research::proto::StatisticsRequest::TRADING_DAY
                            ? 4
                            : 2)
                    * (task.statistics().request().end_index()
                        - task.statistics().request().begin_index()));
                break;
            case wire::FACTOR:
                describe_factor(task);
                break;
            case wire::BACKTEST:
                describe_backtest(task);
                break;
            default:
                throw std::logic_error("research task kind has no description");
        }
    }

    void verify_input(const wire::Task& task) const override
    {
        const bool variable = task.has_factor() && task.factor().has_variable_definition();
        if (task.has_variable_definition() != variable)
            throw std::invalid_argument("research variable differs from its fixed input");
        if (variable)
            protocol::verify_variable_reference(
                task.variable_definition(),
                task.factor().variable_definition());
    }

    void write_manifest(const wire::Task& task, Json& manifest) const override
    {
        manifest["provider_artifact"] = task.provider_artifact();
        manifest["download_authorization"] = task.download_authorization();
        manifest["program_artifact"] = task.program_artifact();
        if (task.has_strategy_use())
            manifest["strategy_use"] = protocol::decode_strategy_use(task.strategy_use());
        if (task.has_variable_definition())
            manifest["variable_definition"] =
                protocol::decode_variable_reference(task.variable_definition());
    }

    void read_manifest(
        const Json& manifest,
        wire::Task& task,
        const std::string& instance) const override
    {
        auto fields = manifest;
        fields.erase("strategy_use");
        fields.erase("variable_definition");
        fields.erase("program_artifact");
        require_fields(fields, {"provider_artifact", "download_authorization"});
        if (!manifest.at("provider_artifact").get<std::string>().empty()
            || !manifest.at("download_authorization").get<std::string>().empty())
            throw std::invalid_argument("unexpected task provider artifact");
        task.set_program_artifact(manifest.at("program_artifact").get<std::string>());
        if (!task.program_artifact().empty() || task.kind() == wire::STATISTICS
            || task.kind() == wire::PERFORMANCE_EVALUATION || task.kind() == wire::SAMPLE
            || task.kind() == wire::LOGIC_TRIAL)
            validate_digest(task.program_artifact());
        if (manifest.contains("strategy_use")) {
            *task.mutable_strategy_use() =
                protocol::encode_strategy_use(manifest.at("strategy_use"));
            if (task.kind() != wire::BACKTEST
                || task.strategy_use().package().owner().service_id() != instance)
                throw std::invalid_argument("invalid fixed strategy use owner or task kind");
        }
        if (manifest.contains("variable_definition")) {
            *task.mutable_variable_definition() =
                protocol::encode_variable_reference(manifest.at("variable_definition"));
            if (task.kind() != wire::FACTOR
                || task.variable_definition().owner().service_id() != instance)
                throw std::invalid_argument("invalid fixed research variable owner or task kind");
        }
    }

    wire::TaskExecution execution(const wire::Task& task, const std::string& digest) const override
    {
        return protocol::task_execution(task, digest);
    }

    void admitted_execution(const wire::Task& task, wire::TaskExecution& execution) const override
    {
        execution.set_program_artifact(task.program_artifact());
        if (task.has_strategy_use())
            *execution.mutable_strategy_use() = task.strategy_use();
    }

    void validate_preparation(
        const wire::PreparedTaskSubmission& value,
        const std::string& instance) const override
    {
        const auto& record = value.record();
        const auto& task = record.task();
        if (task.strategy_use().SerializeAsString()
            != value.execution().strategy_use().SerializeAsString())
            throw std::invalid_argument("task preparation strategy use mismatch");
        if (task.has_strategy_use()) {
            protocol::validate_strategy_use(task.strategy_use());
            if (task.kind() != wire::BACKTEST
                || task.strategy_use().package().owner().service_id() != instance)
                throw std::invalid_argument("invalid fixed strategy use owner or task kind");
        }
        if (task.has_variable_definition() != value.execution().has_variable_definition())
            throw std::invalid_argument("task preparation research variable mismatch");
        if (task.has_variable_definition()) {
            protocol::verify_variable_reference(
                task.variable_definition(),
                value.execution().variable_definition());
            if (task.kind() != wire::FACTOR
                || task.variable_definition().owner().service_id() != instance)
                throw std::invalid_argument("invalid fixed research variable owner or task kind");
        }
        if (task.kind() == wire::PERFORMANCE_EVALUATION) {
            if (!value.execution().has_evaluation()
                || value.execution().program_artifact() != task.program_artifact()
                || value.execution().evaluation().request().source().owner().service_id()
                    != instance)
                throw std::invalid_argument(
                    "evaluation preparation differs from its fixed source or program");
            (void)protocol::task_data_references(value.execution());
        } else if (value.execution().has_evaluation())
            throw std::invalid_argument("unexpected evaluation preparation parameters");
    }

    void admit(tasks::Admission& admission) const override
    {
        auto& task = admission.task;
        if (task.kind() == wire::SAMPLE || task.kind() == wire::LOGIC_TRIAL)
            require_program(task, "calculation requires its fixed execution program");
        if (task.kind() == wire::PERFORMANCE_EVALUATION) {
            require_program(task, "evaluation requires its fixed execution program");
            if (!task.metadata().has_derivation())
                throw std::invalid_argument("invalid experiment derivation");
        }
        if (task.kind() == wire::STATISTICS) {
            require_program(task, "statistics require its fixed execution program");
            if (!task.metadata().has_derivation())
                throw std::invalid_argument("invalid experiment derivation");
        }
        if (task.metadata().has_derivation())
            admit_derivation(admission);
        if (task.has_variable_definition()
            && task.variable_definition().owner().service_id() != admission.instance)
            throw std::invalid_argument("invalid fixed research variable owner or task kind");
        if (const auto* existing = admission.existing) {
            if (task.strategy_use().SerializeAsString()
                != existing->task().strategy_use().SerializeAsString())
                throw Error(ErrorCode::conflict, "task strategy use changed");
            if (task.variable_definition().SerializeAsString()
                != existing->task().variable_definition().SerializeAsString())
                throw Error(ErrorCode::conflict, "task research variable changed");
            return;
        }
        // Validate the fixed definition before reserving new work. A prepared-file
        // execution descriptor is not available at this admission boundary.
        if (task.has_input())
            protocol::require_backtest_scoring_assumptions(
                static_cast<std::size_t>(task.input().strategies_size()),
                task.input().performance_assumptions());
    }

    void confirm_admission(const wire::Task& task, const tasks::Records& records) const override
    {
        if (task.kind() != wire::PERFORMANCE_EVALUATION)
            return;
        const auto& source = task.evaluation().request().source();
        const auto original = records.find(source.id()).task();
        if (records.retired(source.id()) || original.state() != wire::SUCCEEDED
            || original.result_digest() != source.version())
            throw Error(
                ErrorCode::conflict,
                "evaluation source is no longer available for admission");
    }

    bool same_submission(const wire::Task& admitted, const wire::Task& submitted) const override
    {
        return admitted.strategy_use().SerializeAsString()
            == submitted.strategy_use().SerializeAsString();
    }

    void validate_result(
        const wire::Task& task,
        const wire::TaskFinish& result,
        const std::string&) const override
    {
        switch (task.kind()) {
            case wire::BACKTEST:
                protocol::validate_backtest_result(task.input(), result.result());
                break;
            case wire::FACTOR:
                protocol::validate_factor_result(task.factor(), result.factor());
                break;
            case wire::LOGIC_TRIAL:
                protocol::validate_logic_trial_result(task.logic_trial(), result.logic_trial());
                break;
            case wire::SAMPLE:
                protocol::validate_sample_result(task.sample(), result.sample());
                break;
            case wire::PERFORMANCE_EVALUATION:
                protocol::validate_evaluation_result(task.evaluation(), result.evaluation());
                break;
            case wire::STATISTICS:
                protocol::validate_statistics_result(task.statistics(), result.statistics());
                break;
            default:
                throw std::logic_error("research task kind has no result");
        }
    }

    void history_references(
        const wire::Task& task,
        const std::string& dataset_id,
        data::proto::HistoryUsage& usage) const override
    {
        bool market = false;
        const auto inspect = [&](const data::proto::BarDataset& data) {
            market |= std::ranges::find(data.source_dataset_ids(), dataset_id)
                != data.source_dataset_ids().end();
        };
        auto kind = data::proto::HISTORY_REFERENCE_UNSPECIFIED;
        if (task.has_input()) {
            kind = data::proto::HISTORY_BACKTEST;
            for (const auto& contract : task.input().paper().contracts())
                inspect(contract.dataset());
        } else if (task.has_sample()) {
            kind = data::proto::HISTORY_SAMPLE;
            inspect(task.sample().dataset());
        } else if (task.has_factor()) {
            kind = data::proto::HISTORY_FACTOR;
            for (const auto& series : task.factor().series()) {
                if (series.has_bars())
                    inspect(series.bars());
                for (const auto& month : series.dominant().months())
                    inspect(month);
            }
        }
        if (!market)
            return;
        auto* row = usage.add_references();
        row->set_id(task.id());
        row->set_kind(kind);
        row->add_roles(data::proto::HISTORY_MARKET);
    }
};

} // namespace

const tasks::TaskDomain& task_domain()
{
    static const ResearchTasks domain;
    return domain;
}

resource::proto::Catalogue task_resources(
    const tasks::Store& store,
    const std::string& node,
    const resource::proto::CatalogueQuery& query)
{
    const auto limit = protocol::catalogue_limit(query);
    validate_id(node);
    const bool output = query.kind().ends_with("_result");
    const auto name = output ? query.kind().substr(0, query.kind().size() - 7) : query.kind();
    const auto kind =
        std::ranges::find(research_kinds, std::string_view(name), &tasks::KindSpec::resource);
    if (kind == research_kinds.end())
        throw Error(ErrorCode::invalid_request, "resource kind is not implemented by Research");
    resource::proto::Catalogue result;
    auto* owner = result.mutable_owner();
    owner->set_node_id(node);
    owner->set_service_id(store.instance());
    owner->set_domain("research");
    result.set_kind(query.kind());
    unsigned inspected = 0;
    store.visit(query.after(), [&](const wire::StoredTaskRecord& record) {
        if (++inspected > 100000)
            throw Error(ErrorCode::resource_exhausted, "Research resource scan exceeds capacity");
        const auto& task = record.task();
        if (task.kind() != kind->kind || (output && task.state() != wire::SUCCEEDED))
            return true;
        if (result.items_size() == static_cast<int>(limit)) {
            result.set_next_after(result.items(result.items_size() - 1).object().id());
            return false;
        }
        auto* row = result.add_items();
        auto* object = row->mutable_object();
        *object->mutable_owner() = *owner;
        object->set_kind(query.kind());
        object->set_id(task.id());
        object->set_version(output ? task.result_digest() : record.input_digest());
        row->set_state(
            task.has_retirement() ? resource::proto::CatalogueEntry::RETIRED
                                  : resource::proto::CatalogueEntry::REGISTERED);
        row->set_name(task.metadata().name());
        if (output)
            row->set_relative_location(
                (fs::path(task.id()) / "results" / (std::to_string(task.attempt()) + ".pb"))
                    .generic_string());
        else {
            row->set_relative_location((fs::path(task.id()) / "input.pb").generic_string());
            row->set_logical_bytes(record.input_bytes());
        }
        return true;
    });
    return result;
}

} // namespace asterion::research
