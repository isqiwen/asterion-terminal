#include "research_data.hpp"
#include <asterion/protocol/task_execution.hpp>
#include <asterion/protocol/notice_review.hpp>
#include <asterion/protocol/intelligence.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/protocol/service_discovery.hpp>
#include <asterion/kernel/trace.hpp>
#include <algorithm>
#include <fstream>
#include <set>
#include <asterion/kernel/durable_file.hpp>

namespace asterion::tasks {

intelligence::proto::NoticeConsumerBinding
notice_consumer_binding(const task::proto::TaskExecution& execution, const std::string& id)
{
    validate_id(id);
    protocol::validate_message(execution);
    if ((!execution.has_notice_review() && !execution.has_event_set_review())
        || execution.has_data_owner() || execution.input_sha256().size() != 64
        || execution.input_sha256().find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("notice custody requires fixed Research execution");
    intelligence::proto::NoticeConsumerBinding binding;
    if (execution.has_event_set_review()) {
        const auto& fixed = execution.event_set_review();
        protocol::validate_event_set_review_execution(fixed);
        *binding.mutable_source() = fixed.request().source();
        auto& consumer = *binding.mutable_consumer();
        *consumer.mutable_owner() = fixed.owner();
        consumer.set_kind("event_set_review");
        consumer.set_id(id);
        consumer.set_version(execution.input_sha256());
        binding.set_use_scope(fixed.request().use_scope());
        protocol::validate_notice_consumer_binding(binding);
        return binding;
    }
    const auto& fixed = execution.notice_review();
    protocol::validate_notice_review_execution(fixed);
    *binding.mutable_source() = fixed.request().source();
    auto& consumer = *binding.mutable_consumer();
    *consumer.mutable_owner() = fixed.owner();
    consumer.set_kind("notice_review");
    consumer.set_id(id);
    consumer.set_version(execution.input_sha256());
    binding.set_use_scope(fixed.request().use_scope());
    protocol::validate_notice_consumer_binding(binding);
    return binding;
}

std::optional<intelligence::proto::NoticeConsumerRecord> read_notice_custody(
    const std::filesystem::path& directory,
    const intelligence::proto::NoticeConsumerBinding& binding)
{
    namespace fs = std::filesystem;
    protocol::validate_notice_consumer_binding(binding);
    if (directory.filename() != binding.consumer().id() || fs::is_symlink(directory))
        throw std::invalid_argument("notice custody directory identity differs");
    const auto file = directory / "notice-custody.pb";
    if (!fs::exists(file) && !fs::is_symlink(file))
        return {};
    if (fs::is_symlink(file) || !fs::is_regular_file(file) || fs::file_size(file) > 16384)
        throw std::invalid_argument("notice custody receipt is missing or invalid");
    std::ifstream stream(file, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
    intelligence::proto::NoticeConsumerRecord record;
    if (stream.bad() || !record.ParseFromString(bytes))
        throw std::invalid_argument("notice custody receipt is corrupt");
    protocol::validate_notice_consumer_record(record);
    if (record.binding().SerializeAsString() != binding.SerializeAsString())
        throw Error(ErrorCode::conflict, "notice custody receipt belongs to another fixed input");
    return record;
}

void save_notice_custody(
    const std::filesystem::path& directory,
    const intelligence::proto::NoticeConsumerRecord& record)
{
    namespace fs = std::filesystem;
    protocol::validate_notice_consumer_record(record);
    if (!fs::is_directory(directory))
        throw std::invalid_argument("notice custody requires its existing task directory");
    if (const auto old = read_notice_custody(directory, record.binding())) {
        if (old->created_at_ms() != record.created_at_ms())
            throw Error(ErrorCode::conflict, "notice custody original acknowledgement changed");
        if (old->phase() == intelligence::proto::NoticeConsumerRecord::RELEASED
            && record.phase() != old->phase())
            throw Error(ErrorCode::conflict, "released notice custody cannot revive");
        if (old->phase() == intelligence::proto::NoticeConsumerRecord::CONFIRMED
            && record.phase() == intelligence::proto::NoticeConsumerRecord::PREPARED)
            return; // A late preparation reply cannot erase the confirmed acknowledgement.
        if (record.updated_at_ms() < old->updated_at_ms())
            throw Error(ErrorCode::conflict, "notice custody acknowledgement moved backwards");
    }
    replace_file_durably(directory / "notice-custody.pb", record.SerializeAsString());
}

ResearchDataRoute
resolve_research_data(const node::proto::Status& status, const data::proto::DataOwner& selected)
{
    if (status.phase() != node::proto::Status::READY || status.has_failure())
        throw Error(ErrorCode::unavailable, "data discovery is not ready");
    validate_id(status.node_id());
    if (!selected.node_id().empty() && selected.node_id() != status.node_id())
        throw Error(ErrorCode::conflict, "data owner requires another node resolver");
    if (!selected.service_id().empty())
        validate_id(selected.service_id());
    const node::proto::Service* candidate = nullptr;
    for (const auto& service : status.services()) {
        if (service.kind() != node::proto::DATA
            || (!selected.service_id().empty() && service.id() != selected.service_id()))
            continue;
        if (candidate)
            throw Error(ErrorCode::conflict, "multiple data owners require an explicit selection");
        candidate = &service;
    }
    if (!candidate)
        throw Error(ErrorCode::not_found, "selected data owner is not deployed");
    validate_id(candidate->id());
    if (candidate->state() != "running" || candidate->endpoint().empty())
        throw Error(ErrorCode::unavailable, "selected data owner is unavailable");
    ResearchDataRoute route;
    route.owner.set_node_id(status.node_id());
    route.owner.set_service_id(candidate->id());
    route.endpoint = candidate->endpoint();
    route.research_node_id = status.node_id();
    return route;
}

ResearchFactsRoute
resolve_research_facts(const node::proto::Status& status, const resource::proto::Owner& selected)
{
    if (status.phase() != node::proto::Status::READY || status.has_failure())
        throw Error(ErrorCode::unavailable, "trading facts discovery is not ready");
    validate_id(status.node_id());
    validate_id(selected.node_id());
    validate_id(selected.service_id());
    if (selected.domain() != "trading")
        throw Error(ErrorCode::invalid_request, "fixed facts require a trading owner");
    if (selected.node_id() != status.node_id())
        throw Error(ErrorCode::conflict, "trading facts owner requires another node resolver");
    for (const auto& service : status.services()) {
        if (service.id() != selected.service_id())
            continue;
        if (service.kind() != node::proto::TRADING)
            throw Error(ErrorCode::conflict, "selected facts owner is not trading");
        if (service.state() != "running" || service.endpoint().empty())
            throw Error(ErrorCode::unavailable, "selected trading facts owner is unavailable");
        return {selected, service.endpoint()};
    }
    throw Error(ErrorCode::not_found, "selected trading facts owner is not deployed");
}

ResearchFactsRoute
resolve_research_notice(const node::proto::Status& status, const resource::proto::Owner& selected)
{
    if (status.phase() != node::proto::Status::READY || status.has_failure())
        throw Error(ErrorCode::unavailable, "notice publication discovery is not ready");
    validate_id(status.node_id());
    validate_id(selected.node_id());
    validate_id(selected.service_id());
    if (selected.domain() != "intelligence")
        throw Error(
            ErrorCode::invalid_request,
            "notice publication requires an Intelligence owner");
    if (selected.node_id() != status.node_id())
        throw Error(ErrorCode::conflict, "cross-node notice publication transfer is unavailable");
    const node::proto::Service* found = nullptr;
    for (const auto& service : status.services()) {
        if (service.id() != selected.service_id())
            continue;
        if (found || service.kind() != node::proto::INTELLIGENCE)
            throw Error(ErrorCode::conflict, "selected notice owner is not unique Intelligence");
        found = &service;
    }
    if (!found)
        throw Error(ErrorCode::not_found, "selected notice owner is not deployed");
    if (found->state() != "running" || found->endpoint().empty())
        throw Error(ErrorCode::unavailable, "selected notice publication owner is unavailable");
    return {selected, found->endpoint()};
}

ResearchProgram resolve_research_program(
    const node::proto::Status& status,
    const std::string& id,
    std::uint64_t pid)
{
    if (status.phase() != node::proto::Status::READY || status.has_failure())
        throw Error(ErrorCode::unavailable, "research program discovery is not ready");
    validate_id(status.node_id());
    validate_id(id);
    for (const auto& service : status.services()) {
        if (service.id() != id)
            continue;
        if (service.kind() != node::proto::RESEARCH || !pid || service.pid() != pid)
            throw Error(
                ErrorCode::conflict,
                "research program observation belongs to another process");
        if (service.state() != "running")
            throw Error(ErrorCode::unavailable, "research program is unavailable");
        if (service.artifact().size() != 64
            || !std::all_of(service.artifact().begin(), service.artifact().end(), [](char c) {
                   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
               }))
            throw Error(ErrorCode::conflict, "research program artifact is not fixed");
        ResearchProgram result;
        result.owner.set_domain("research");
        result.owner.set_node_id(status.node_id());
        result.owner.set_service_id(id);
        result.artifact = service.artifact();
        return result;
    }
    throw Error(ErrorCode::not_found, "research program is not deployed");
}

data::proto::DataRetention research_retention(
    const task::proto::TaskExecution& execution,
    const ResearchDataRoute& route,
    const std::string& service,
    const std::string& id)
{
    validate_id(route.research_node_id);
    validate_id(service);
    validate_id(id);
    if (!execution.has_data_owner()
        || execution.data_owner().SerializeAsString() != route.owner.SerializeAsString())
        throw Error(ErrorCode::conflict, "task retention source owner changed");
    data::proto::DataRetention result;
    *result.mutable_source() = execution.data_owner();
    auto* consumer = result.mutable_consumer();
    consumer->mutable_owner()->set_domain("research");
    consumer->mutable_owner()->set_node_id(route.research_node_id);
    consumer->mutable_owner()->set_service_id(service);
    consumer->set_id(id);
    consumer->set_version(execution.input_sha256());
    consumer->set_kind(
        execution.has_backtest()     ? "backtest"
            : execution.has_sample() ? "sample"
                                     : "factor");
    const auto references = protocol::task_data_references(execution);
    for (const auto& id : references.minutes) {
        auto* version = result.add_versions();
        version->set_kind(data::proto::DataVersion::MINUTE_VERSION);
        version->set_version(id);
    }
    for (const auto& id : references.snapshots) {
        auto* version = result.add_versions();
        version->set_kind(data::proto::DataVersion::TABLE_SNAPSHOT);
        version->set_version(id);
    }
    return result;
}

void verify_research_retention(
    const std::filesystem::path& directory,
    const data::proto::DataRetention& expected)
{
    const auto path = directory / "data-retention.pb";
    if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path)
        || std::filesystem::file_size(path) > 512 * 1024)
        throw std::invalid_argument("task has no supported data retention intent");
    std::ifstream stream(path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
    data::proto::DataRetention stored;
    if (stream.bad() || !stored.ParseFromString(bytes))
        throw std::invalid_argument("invalid task data retention intent");
    protocol::validate_message(stored);
    if (stored.SerializeAsString() != expected.SerializeAsString())
        throw Error(ErrorCode::conflict, "task data retention intent differs from fixed input");
}

void save_research_retention(
    const std::filesystem::path& directory,
    const data::proto::DataRetention& value)
{
    const auto path = directory / "data-retention.pb";
    if (std::filesystem::exists(path)) {
        verify_research_retention(directory, value);
        sync_directory(directory);
    } else
        replace_file_durably(path, value.SerializeAsString());
}

ResearchData::ResearchData(std::string discovery_endpoint)
    : discovery_(std::move(discovery_endpoint), 8, PayloadBudget{4 * 1024 * 1024}, 4 * 1024 * 1024)
{}

PolledTask<ResearchDataRoute> ResearchData::resolve(
    data::proto::DataOwner selected,
    std::chrono::milliseconds timeout,
    std::string parent)
{
    co_return resolve_research_data(co_await observe(timeout, std::move(parent)), selected);
}

PolledTask<ResearchFactsRoute> ResearchData::resolve_facts(
    resource::proto::Owner selected,
    std::chrono::milliseconds timeout,
    std::string parent)
{
    co_return resolve_research_facts(co_await observe(timeout, std::move(parent)), selected);
}

PolledTask<ResearchFactsRoute> ResearchData::resolve_notice(
    resource::proto::Owner selected,
    std::chrono::milliseconds timeout,
    std::string parent)
{
    co_return resolve_research_notice(co_await observe(timeout, std::move(parent)), selected);
}

PolledTask<ResearchProgram> ResearchData::program(
    std::string service,
    std::uint64_t pid,
    std::chrono::milliseconds timeout,
    std::string parent)
{
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            throw Error(
                ErrorCode::unavailable,
                "research program observation is not ready for this process");
        const auto status =
            co_await observe(std::chrono::ceil<std::chrono::milliseconds>(deadline - now), parent);
        validate_id(status.node_id());
        if (status.has_failure())
            throw Error(ErrorCode::unavailable, "research program discovery is not ready");
        for (const auto& candidate : status.services()) {
            if (candidate.id() != service)
                continue;
            if (candidate.kind() != node::proto::RESEARCH)
                throw Error(
                    ErrorCode::conflict,
                    "research program observation belongs to another process");
            if (status.phase() == node::proto::Status::READY && candidate.pid() == pid
                && candidate.state() == "running")
                co_return resolve_research_program(status, service, pid);
        }
        // A starting child can query before Agent publishes its new PID.
        // Repeat only this read; never accept an older process or retry execution.
        const auto next = std::min(deadline, std::chrono::steady_clock::now() + 25ms);
        co_await PollUntil{[next] {
            return std::chrono::steady_clock::now() >= next;
        }};
    }
}

PolledTask<node::proto::Status>
ResearchData::observe(std::chrono::milliseconds timeout, std::string parent)
{
    const auto request = protocol::service_discovery_request(parent);
    const auto raw = co_await PollFuture{discovery_.request(request.SerializeAsString(), timeout)};
    co_return protocol::decode_service_discovery(request, *raw);
}

PolledTask<Payload> ResearchData::read(
    const ResearchDataRoute& route,
    std::string request,
    std::chrono::milliseconds timeout)
{
    co_return co_await read_endpoint(route.endpoint, std::move(request), timeout);
}

PolledTask<Payload> ResearchData::read(
    const ResearchFactsRoute& route,
    std::string request,
    std::chrono::milliseconds timeout)
{
    co_return co_await read_endpoint(route.endpoint, std::move(request), timeout);
}

PolledTask<Payload> ResearchData::read_endpoint(
    std::string endpoint,
    std::string request,
    std::chrono::milliseconds timeout)
{
    std::erase_if(readers_, [](const auto& reader) {
        return reader.expired();
    });
    if (readers_.size() >= 8)
        throw Error(ErrorCode::resource_exhausted, "research data read capacity exhausted");
    auto reader = std::make_shared<ipc::RpcClient>(std::move(endpoint), 1, payloads_);
    readers_.push_back(reader);
    co_return co_await PollFuture{reader->request(std::move(request), timeout)};
}

void ResearchData::poll()
{
    discovery_.poll();
    std::erase_if(readers_, [](const auto& reader) {
        return reader.expired();
    });
    for (const auto& reader : readers_)
        if (const auto active = reader.lock())
            active->poll();
}

} // namespace asterion::tasks
