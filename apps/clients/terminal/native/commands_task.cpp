#include <set>
#include "quantstats.hpp"
#include <asterion/kernel/environment.hpp>
#include "research_export.hpp"
#include "application_impl.hpp"
#include <asterion/protocol/factor.hpp>

namespace asterion::terminal {

void Application::Impl::register_task_commands()
{
    command("task.page", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"before_sequence"});
        const auto& before = p.at("before_sequence");
        if (!before.is_number_integer() || before < 0 || before > 4294967295ULL)
            throw std::invalid_argument("invalid task page cursor");
        if (!task_client)
            throw std::invalid_argument("task service is not connected");
        const auto client = task_client;
        (co_await PollFuture{client->page(before.get<unsigned>())});
        if (task_client != client)
            throw Error(ErrorCode::conflict, "task service changed during page query");
        co_return snapshot();
    });
    command("task.action", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "action"});
        if (!task_client)
            throw std::invalid_argument("task service is not connected");
        const auto client = task_client;
        const auto generation = data_task_generation;
        (co_await PollFuture{client->action(text(p, "id"), text(p, "action"))});
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/task service selection changed during operation; inspect the " "original services before retrying");
        co_return snapshot();
    });
    command("task.comparison.export", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"expected", "matrix", "path"});
        if (!task_client)
            throw std::invalid_argument("task service is not connected");
        const auto& expected = p.at("expected");
        if (!expected.is_array() || expected.size() < 2 || expected.size() > 3)
            throw std::invalid_argument("invalid research comparison evidence");
        const auto client = task_client;
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
                    "data/task service selection changed during operation; " "inspect the original services before retrying");
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
                {"sha256", sha256_bytes(content)},
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
    command("task.evidence.export", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "path"});
        if (!task_client)
            throw std::invalid_argument("task service is not connected");
        const auto client = task_client;
        const auto generation = data_task_generation;
        const auto instance = client->owner_view().connection.at("service").get<std::string>();
        auto source = co_await PollFuture{client->result(text(p, "id"))};
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/task service selection changed during operation; " "inspect the original services before retrying");
        const auto path = p.at("path").get<std::string>();
        auto exported =
            co_await service_io.admin<json>([source = std::move(source), path, instance] {
                const auto content = research_evidence_document(source, instance);
                json receipt = {
                    {"name", source.at("id").get<std::string>() + ".evidence.json"},
                    {"sha256", sha256_bytes(content)},
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
        fields(p, {"id"});
        if (!task_client)
            throw std::invalid_argument("task service is not connected");
        const auto client = task_client;
        const auto generation = data_task_generation;
        auto task = co_await PollFuture{client->metadata(text(p, "id"))};
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/task service selection changed during operation; " "inspect the original services before retrying");
        auto reply = snapshot();
        reply.values["task_description"] = std::move(task);
        co_return reply;
    });
    command("task.metadata.edit", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "expected_revision", "name", "note"});
        if (!task_client)
            throw std::invalid_argument("task service is not connected");
        const auto& revision = p.at("expected_revision");
        if (!revision.is_number_integer() || revision < 0 || revision > 4294967295ULL
            || !p.at("name").is_string() || !p.at("note").is_string())
            throw std::invalid_argument("invalid experiment metadata");
        task::v1::TaskMetadataEdit edit;
        edit.set_id(text(p, "id"));
        edit.set_expected_revision(revision.get<unsigned>());
        edit.set_name(p.at("name").get<std::string>());
        edit.set_note(p.at("note").get<std::string>());
        const auto client = task_client;
        const auto generation = data_task_generation;
        co_await PollFuture{client->edit_metadata(edit)};
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/task service selection changed during operation; " "inspect the original services before retrying");
        co_return snapshot();
    });
    command("task.quantstats.analyze", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id", "result_digest"});
        if (!task_client)
            throw std::invalid_argument("task service is not connected");
        validate_id(text(p, "id"));
        const auto client = task_client;
        const auto generation = data_task_generation;
        auto source = co_await PollFuture{client->result(text(p, "id"))};
        if (source.at("task").at("result_digest") != text(p, "result_digest"))
            throw Error(
                ErrorCode::conflict,
                "backtest result changed; reopen the result before analysis");
        auto report = co_await service_io.admin<json>([source = std::move(source)] {
            return quantstats_report(
                source,
                environment_path("ASTERION_QUANTSTATS_EXECUTABLE")
                    .value_or(std::filesystem::path{}));
        });
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/task service selection changed during operation; inspect the original services before retrying");
        auto reply = snapshot();
        reply.values["quantstats_report"] = std::move(report);
        co_return reply;
    });
    command("task.result.read", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id"});
        if (!task_client)
            throw std::invalid_argument("task service is not connected");
        validate_id(text(p, "id"));
        const auto client = task_client;
        const auto generation = data_task_generation;
        auto evidence = (co_await PollFuture{client->result(text(p, "id"))});
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/task service selection changed during operation; inspect the " "original services before retrying");
        auto reply = snapshot();
        reply.values["task_result"] = std::move(evidence);
        co_return reply;
    });
    command("task.result", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"id"});
        if (!task_client)
            throw std::invalid_argument("task service is not connected");
        validate_id(text(p, "id"));
        const auto client = task_client;
        const auto generation = data_task_generation;
        auto evidence = (co_await PollFuture{client->result(text(p, "id"))});
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/task service selection changed during operation; inspect the " "original services before retrying");
        task_result = std::make_shared<const json>(std::move(evidence));
        co_return snapshot();
    });
}

} // namespace asterion::terminal
