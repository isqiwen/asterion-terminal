#include "observation_store.hpp"
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/protocol/trading.hpp>
#include <fstream>
#include <algorithm>
#include <chrono>

namespace asterion::data {
namespace fs = std::filesystem;

namespace {

void directory(const fs::path& path)
{
    if (fs::is_symlink(path) || !fs::is_directory(path))
        throw std::invalid_argument("invalid observation archive directory");
}

template <class T>
T read_message(const fs::path& path, std::size_t maximum)
{
    directory(path.parent_path());
    if (fs::is_symlink(path) || !fs::is_regular_file(path) || fs::file_size(path) > maximum)
        throw std::invalid_argument("observation archive evidence is missing or invalid");
    std::ifstream input(path, std::ios::binary);
    T value;
    if (!value.ParseFromIstream(&input) || input.bad())
        throw std::invalid_argument("invalid observation archive protobuf");
    protocol::validate_message(value);
    return value;
}

std::int64_t observed_time_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void cancelled(std::stop_token stop)
{
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "observation archiving cancelled");
}

} // namespace

Observations::Observations(fs::path root, resource::proto::Owner owner)
    : root_(std::move(root) / "observations"),
      owner_(std::move(owner))
{
    if (!root_.is_absolute())
        throw std::invalid_argument("invalid observation archive directory");
    validate_id(owner_.node_id());
    validate_id(owner_.service_id());
    if (owner_.domain() != "data")
        throw std::invalid_argument("invalid observation archive owner");
    if (fs::is_symlink(root_))
        throw std::invalid_argument("invalid observation archive directory");
}

bool Observations::authority() const
{
    const auto guard = root_.parent_path() / "observations.required.pb";
    if (!fs::exists(guard) && !fs::is_symlink(guard)) {
        if (fs::exists(root_) || fs::is_symlink(root_))
            throw std::invalid_argument("observation archive operation authority is missing");
        return false;
    }
    const auto saved = read_message<resource::proto::Owner>(guard, 4096);
    if (saved.SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "observation archive owner mismatch");
    if (fs::is_symlink(root_) || !fs::is_directory(root_) || fs::is_symlink(root_ / "operations")
        || !fs::is_directory(root_ / "operations"))
        throw std::invalid_argument("observation archive operation authority is missing");
    return true;
}

fs::path Observations::operation_file(const std::string& id) const
{
    validate_id(id);
    return root_ / "operations" / (id + ".pb");
}

fs::path Observations::object_file(const std::string& version) const
{
    if (version.size() != 64 || version.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid observation archive identity");
    return root_ / "objects" / (version + ".pb");
}

market::proto::ObservationCapture
Observations::capture_file(const fs::path& path, const resource::proto::Object& expected) const
{
    directory(root_);
    auto value =
        read_message<market::proto::ObservationCapture>(path, protocol::max_observation_bytes);
    protocol::validate_observation(value);
    if (value.reference().SerializeAsString() != expected.SerializeAsString())
        throw Error(ErrorCode::conflict, "observation payload identity mismatch");
    return value;
}

proto::ObservationStatus Observations::read(const std::string& id, bool payload) const
{
    const auto file = operation_file(id);
    if (!authority())
        throw Error(ErrorCode::not_found, "unknown observation operation");
    if (!fs::exists(file) && !fs::is_symlink(file))
        throw Error(ErrorCode::not_found, "unknown observation operation");
    directory(root_);
    proto::ObservationStatus result;
    *result.mutable_operation() = read_message<proto::ObservationOperation>(file, 65536);
    (void)protocol::decode_observation_status(result);
    const auto& operation = result.operation();
    if (operation.id() != id || operation.source().owner().node_id() != owner_.node_id()
        || (operation.has_archive()
            && operation.archive().owner().SerializeAsString() != owner_.SerializeAsString()))
        throw Error(ErrorCode::conflict, "observation archive owner mismatch");
    if (payload && operation.state() == proto::ObservationOperation::PUBLISHED)
        *result.mutable_capture() =
            capture_file(object_file(operation.archive().id()), operation.source());
    return result;
}

void Observations::validate_admission(const proto::ObservationAdmission& request) const
{
    protocol::validate_message(request);
    protocol::validate_observation_reference(request.source());
    const auto file = operation_file(request.id());
    if (request.source().owner().node_id() != owner_.node_id())
        throw Error(ErrorCode::conflict, "cross-node observation archiving is not available");
    (void)authority();
    if (fs::exists(file) || fs::is_symlink(file)) {
        const auto existing = read(request.id(), false).operation();
        if (existing.source().SerializeAsString() != request.source().SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "observation operation identity has different content");
    }
}

proto::ObservationOperation Observations::admit(const proto::ObservationAdmission& request) const
{
    validate_admission(request);
    const auto file = operation_file(request.id());
    if (fs::exists(file) || fs::is_symlink(file)) {
        auto existing = read(request.id(), false).operation();
        sync_file_durably(file);
        return existing;
    }
    create_directories_durably(root_ / "operations");
    const auto guard = root_.parent_path() / "observations.required.pb";
    if (!fs::exists(guard))
        replace_file_durably(guard, owner_.SerializeAsString());
    else
        sync_file_durably(guard);
    proto::ObservationOperation operation;
    operation.set_id(request.id());
    *operation.mutable_source() = request.source();
    operation.set_state(proto::ObservationOperation::ADMITTED);
    operation.set_admitted_at_ms(observed_time_ms());
    replace_file_durably(file, operation.SerializeAsString());
    return operation;
}

Observations::Prepared
Observations::prepare(const std::string& id, const Fetch& fetch, std::stop_token stop) const
{
    cancelled(stop);
    Prepared result;
    result.operation = read(id, false).operation();
    const auto& source = result.operation.source();
    const auto object = object_file(source.version());
    const auto staging = root_ / "staging" / (id + ".pb");
    if (fs::exists(object) || fs::is_symlink(object))
        result.capture = capture_file(object, source);
    else if (fs::exists(staging) || fs::is_symlink(staging))
        result.capture = capture_file(staging, source);
    else {
        if (result.operation.state() == proto::ObservationOperation::PUBLISHED)
            throw std::invalid_argument("published observation archive is missing");
        result.capture = fetch(source, stop);
        protocol::validate_observation(result.capture);
        if (result.capture.reference().SerializeAsString() != source.SerializeAsString())
            throw Error(ErrorCode::conflict, "observation payload identity mismatch");
        cancelled(stop);
        create_directories_durably(root_ / "staging");
        replace_file_durably(staging, result.capture.SerializeAsString());
    }
    cancelled(stop);
    return result;
}

proto::ObservationOperation Observations::publish(const Prepared& prepared) const
{
    auto operation = read(prepared.operation.id(), false).operation();
    if (operation.source().SerializeAsString() != prepared.operation.source().SerializeAsString()
        || prepared.capture.reference().SerializeAsString()
            != operation.source().SerializeAsString())
        throw Error(ErrorCode::conflict, "observation payload identity mismatch");
    protocol::validate_observation(prepared.capture);
    const auto object = object_file(operation.source().version());
    if (fs::exists(object) || fs::is_symlink(object)) {
        (void)capture_file(object, operation.source());
        sync_file_durably(object);
    } else {
        const auto staging = root_ / "staging" / (operation.id() + ".pb");
        (void)capture_file(staging, operation.source());
        create_directories_durably(root_ / "objects");
        publish_file_durably(staging, object);
    }
    if (operation.state() == proto::ObservationOperation::PUBLISHED) {
        sync_file_durably(operation_file(operation.id()));
        return operation;
    }
    auto* archive = operation.mutable_archive();
    *archive->mutable_owner() = owner_;
    archive->set_kind("observation_archive");
    archive->set_id(operation.source().version());
    archive->set_version(archive->id());
    operation.set_state(proto::ObservationOperation::PUBLISHED);
    operation.set_published_at_ms(observed_time_ms());
    replace_file_durably(operation_file(operation.id()), operation.SerializeAsString());
    return operation;
}

std::vector<proto::ObservationOperation> Observations::operations(std::stop_token stop) const
{
    std::vector<proto::ObservationOperation> result;
    if (!authority())
        return result;
    for (const auto& entry : fs::directory_iterator(root_ / "operations")) {
        cancelled(stop);
        if (entry.is_symlink() || !entry.is_regular_file())
            throw std::invalid_argument("invalid observation operation file");
        if (entry.path().filename().string().starts_with(".tmp-")
            && entry.path().extension() != ".pb")
            continue;
        if (entry.path().extension() != ".pb")
            throw std::invalid_argument("invalid observation operation file");
        result.push_back(read(entry.path().stem().string(), false).operation());
        if (result.size() > 100000)
            throw Error(ErrorCode::resource_exhausted, "observation archive scan exceeds capacity");
    }
    std::ranges::sort(result, {}, [](const auto& operation) {
        return operation.id();
    });
    return result;
}

} // namespace asterion::data
