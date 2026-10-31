#include <asterion/protocol/observation.hpp>
#include <asterion/protocol/market.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/base/id.hpp>

namespace asterion::protocol {
namespace {

Json object(const resource::proto::Object& value)
{
    return {
        {"node", value.owner().node_id()},
        {"service", value.owner().service_id()},
        {"domain", value.owner().domain()},
        {"kind", value.kind()},
        {"id", value.id()},
        {"version", value.version()}
    };
}

} // namespace

void validate_observation_reference(const resource::proto::Object& value)
{
    validate_message(value);
    validate_id(value.owner().node_id());
    validate_id(value.owner().service_id());
    validate_id(value.id());
    if (value.owner().domain() != "market" || value.kind() != "market_observation"
        || value.version().size() != 64
        || value.version().find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid market observation identity");
}

void validate_observation(const market::proto::ObservationCapture& value)
{
    validate_message(value);
    validate_observation_reference(value.reference());
    const auto& source = value.observation();
    validate_id(source.instance());
    validate_id(source.token());
    validate_id(source.stream_id());
    if (value.ByteSizeLong() > max_observation_bytes
        || source.source().SerializeAsString() != value.reference().owner().SerializeAsString()
        || source.token() != value.reference().id() || !source.applied_sequence()
        || source.captured_at_ms() <= 0 || source.minutes().first_observation_ms() <= 0
        || source.minutes().first_observation_ms() > source.captured_at_ms()
        || source.minutes().bars_size() == 0 || source.minutes().bars_size() > 4096
        || source.source_phase().empty() || source.source_phase().size() > 64
        || sha256_bytes(source.SerializeAsString()) != value.reference().version())
        throw std::invalid_argument("invalid market observation evidence");
    (void)decode_minutes(source.minutes());
}

Json decode_observation(const market::proto::ObservationCapture& value)
{
    validate_observation(value);
    const auto& source = value.observation();
    return {
        {"reference", object(value.reference())},
        {"instance", source.instance()},
        {"captured_at_ms", std::to_string(source.captured_at_ms())},
        {"stream_id", source.stream_id()},
        {"applied_sequence", std::to_string(source.applied_sequence())},
        {"source_phase", source.source_phase()},
        {"minutes", decode_minutes(source.minutes())},
        {"aggregate_only", true},
        {"complete_history", false}
    };
}

Json decode_observation_status(const data::proto::ObservationStatus& value)
{
    validate_message(value);
    const auto& operation = value.operation();
    validate_id(operation.id());
    validate_observation_reference(operation.source());
    if (operation.admitted_at_ms() <= 0
        || (operation.state() == data::proto::ObservationOperation::ADMITTED
            && operation.published_at_ms() != 0)
        || (operation.state() == data::proto::ObservationOperation::PUBLISHED
            && operation.published_at_ms() <= 0))
        throw std::invalid_argument("invalid observation operation state");
    if (operation.state() != data::proto::ObservationOperation::ADMITTED
        && operation.state() != data::proto::ObservationOperation::PUBLISHED)
        throw std::invalid_argument("invalid observation operation state");
    Json result = {
        {"id", operation.id()},
        {"admitted_at_ms", std::to_string(operation.admitted_at_ms())},
        {"source", object(operation.source())},
        {"state",
            operation.state() == data::proto::ObservationOperation::PUBLISHED ? "published"
                                                                              : "admitted"}
    };
    if (operation.state() == data::proto::ObservationOperation::PUBLISHED) {
        const auto& archive = operation.archive();
        validate_id(archive.owner().node_id());
        validate_id(archive.owner().service_id());
        if (archive.owner().domain() != "data" || archive.kind() != "observation_archive"
            || archive.id() != operation.source().version() || archive.version() != archive.id())
            throw std::invalid_argument("invalid observation archive identity");
        result["archive"] = object(archive);
        result["published_at_ms"] = std::to_string(operation.published_at_ms());
    } else if (operation.has_archive())
        throw std::invalid_argument("unpublished observation has archive identity");
    if (value.has_capture()) {
        if (operation.state() != data::proto::ObservationOperation::PUBLISHED
            || value.capture().reference().SerializeAsString()
                != operation.source().SerializeAsString())
            throw std::invalid_argument("observation payload identity mismatch");
        result["capture"] = decode_observation(value.capture());
    }
    return result;
}

} // namespace asterion::protocol
