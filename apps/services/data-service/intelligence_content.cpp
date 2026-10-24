#include "intelligence_content.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/trading.hpp>
#include <fstream>

namespace asterion::data {
namespace fs = std::filesystem;

namespace {

void directory(const fs::path& p)
{
    if (fs::is_symlink(p) || !fs::is_directory(p))
        throw std::invalid_argument("intelligence content directory unavailable");
}

std::string bytes(const fs::path& p, std::uintmax_t maximum)
{
    directory(p.parent_path());
    if (fs::is_symlink(p) || !fs::is_regular_file(p) || fs::file_size(p) > maximum)
        throw std::invalid_argument("intelligence content evidence missing or invalid");
    std::ifstream in(p, std::ios::binary);
    std::string value{std::istreambuf_iterator<char>(in), {}};
    if (in.bad())
        throw std::runtime_error("intelligence content read failed");
    return value;
}

template <class T>
T message(const fs::path& p, std::uintmax_t maximum)
{
    T value;
    if (!value.ParseFromString(bytes(p, maximum)))
        throw std::invalid_argument("invalid intelligence content metadata");
    return value;
}

std::string key(const resource::proto::Object& origin)
{
    validate_id(origin.owner().node_id());
    validate_id(origin.owner().service_id());
    validate_id(origin.id());
    if (origin.owner().domain() != "intelligence"
        || (origin.kind() != "intelligence_preparation"
            && origin.kind() != "intelligence_change_preparation"
            && origin.kind() != "intelligence_set_preparation")
        || origin.version().size() != 64
        || origin.version().find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid intelligence content origin");
    return sha256_bytes(origin.SerializeAsString());
}

} // namespace

IntelligenceContent::IntelligenceContent(
    fs::path root,
    resource::proto::Owner owner,
    std::shared_ptr<const Clock> clock)
    : root_(std::move(root) / "intelligence-content"),
      owner_(std::move(owner)),
      clock_(std::move(clock))
{
    validate_id(owner_.node_id());
    validate_id(owner_.service_id());
    if (!root_.is_absolute() || owner_.domain() != "data" || !clock_)
        throw std::invalid_argument("invalid intelligence content owner");
    (void)authority();
}

bool IntelligenceContent::authority() const
{
    directory(root_.parent_path());
    const auto guard = root_.parent_path() / "intelligence-content.required.pb";
    if (!fs::exists(guard) && !fs::is_symlink(guard)) {
        if (fs::exists(root_) || fs::is_symlink(root_)) {
            directory(root_);
            for (const auto& e : fs::directory_iterator(root_)) {
                const auto n = e.path().filename().string();
                if ((n != "objects" && n != "receipts") || e.is_symlink() || !e.is_directory()
                    || !fs::is_empty(e.path()))
                    throw std::invalid_argument("intelligence content ownership authority missing");
            }
        }
        return false;
    }
    const auto saved = message<resource::proto::Owner>(guard, 16384);
    if (saved.SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "intelligence content owner mismatch");
    directory(root_);
    directory(root_ / "objects");
    directory(root_ / "receipts");
    return true;
}

std::pair<intelligence::proto::NoticeStorageReceipt, std::string> IntelligenceContent::read_body(
    const resource::proto::Object& origin) const
{
    const auto id = key(origin);
    if (!authority())
        throw Error(ErrorCode::not_found, "unknown intelligence content");
    auto receipt = message<intelligence::proto::NoticeStorageReceipt>(
        root_ / "receipts" / (id + ".pb"),
        16384);
    protocol::validate_notice_storage(receipt);
    if (receipt.origin().SerializeAsString() != origin.SerializeAsString()
        || receipt.content().owner().SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "intelligence content receipt identity mismatch");
    auto body = bytes(root_ / "objects" / origin.version(), 1024 * 1024);
    if (body.size() != receipt.bytes() || sha256_bytes(body) != origin.version())
        throw std::invalid_argument("intelligence content body is corrupt");
    return {std::move(receipt), std::move(body)};
}

IntelligenceContent::Read IntelligenceContent::read(const resource::proto::Object& origin) const
{
    if (origin.kind() != "intelligence_preparation")
        throw std::invalid_argument("ordinary notice reader requires ordinary preparation");
    auto [receipt, body] = read_body(origin);
    Read result;
    result.receipt = std::move(receipt);
    if (!result.payload.ParseFromString(body))
        throw std::invalid_argument("intelligence content payload is invalid");
    protocol::validate_notice_payload(result.payload);
    if (result.payload.owner().SerializeAsString() != origin.owner().SerializeAsString()
        || result.payload.id() != origin.id())
        throw Error(ErrorCode::conflict, "intelligence content body origin mismatch");
    return result;
}

IntelligenceContent::ChangeRead IntelligenceContent::read_change(
    const resource::proto::Object& origin) const
{
    if (origin.kind() != "intelligence_change_preparation")
        throw std::invalid_argument("notice change reader requires change preparation");
    auto [receipt, body] = read_body(origin);
    ChangeRead result;
    result.receipt = std::move(receipt);
    if (!result.payload.ParseFromString(body))
        throw std::invalid_argument("intelligence change payload is invalid");
    intelligence::proto::NoticeChangeIntent intent;
    *intent.mutable_reference() = origin;
    *intent.mutable_payload() = result.payload;
    *intent.mutable_data_owner() = owner_;
    intent.set_prepared_at_ms(result.payload.reviewed_at_ms());
    protocol::validate_notice_change_intent(intent);
    return result;
}

std::optional<IntelligenceContent::ChangeRead> IntelligenceContent::find_change(
    const resource::proto::Object& origin) const
{
    const auto id = key(origin);
    if (!authority())
        return {};
    const auto file = root_ / "receipts" / (id + ".pb");
    if (!fs::exists(file) && !fs::is_symlink(file))
        return {};
    return read_change(origin);
}

IntelligenceContent::SetRead IntelligenceContent::read_set(
    const resource::proto::Object& origin) const
{
    if (origin.kind() != "intelligence_set_preparation")
        throw std::invalid_argument("event set reader requires set preparation");
    auto [receipt, body] = read_body(origin);
    SetRead result;
    result.receipt = std::move(receipt);
    if (!result.payload.ParseFromString(body))
        throw std::invalid_argument("event set content is invalid");
    intelligence::proto::NoticeSetIntent intent;
    *intent.mutable_reference() = origin;
    *intent.mutable_payload() = result.payload;
    *intent.mutable_data_owner() = owner_;
    intent.set_prepared_at_ms(result.payload.request().query().as_of_ms());
    protocol::validate_notice_set_intent(intent);
    return result;
}

std::optional<IntelligenceContent::SetRead> IntelligenceContent::find_set(
    const resource::proto::Object& origin) const
{
    const auto id = key(origin);
    if (!authority())
        return {};
    const auto file = root_ / "receipts" / (id + ".pb");
    if (!fs::exists(file) && !fs::is_symlink(file))
        return {};
    return read_set(origin);
}

void IntelligenceContent::verify_set_members(
    const intelligence::proto::NoticeSetPayload& payload) const
{
    for (const auto& member : payload.members()) {
        const auto& statement = member.publication().publication();
        if (statement.storage().content().owner().SerializeAsString() != owner_.SerializeAsString())
            throw Error(
                ErrorCode::unavailable,
                "event set transfer between Data owners is not implemented");
        const auto saved = read(statement.preparation());
        if (saved.receipt.SerializeAsString() != statement.storage().SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "event set member storage differs from Data authority");
        if (member.event().has_latest_change()) {
            const auto& change = member.event().latest_change();
            if (change.published().storage().content().owner().SerializeAsString()
                != owner_.SerializeAsString())
                throw Error(
                    ErrorCode::unavailable,
                    "event set transfer between Data owners is not implemented");
            const auto saved_change = read_change(change.intent().reference());
            if (saved_change.receipt.SerializeAsString()
                != change.published().storage().SerializeAsString())
                throw Error(
                    ErrorCode::conflict,
                    "event set change storage differs from Data authority");
        }
    }
}

intelligence::proto::NoticeStorageReceipt
IntelligenceContent::save_set(const resource::proto::Object& origin, const FetchSet& fetch)
{
    if (origin.kind() != "intelligence_set_preparation")
        throw std::invalid_argument("event set save requires set preparation");
    if (origin.owner().node_id() != owner_.node_id())
        throw Error(ErrorCode::unavailable, "remote intelligence content transfer unavailable");
    if (const auto saved = find_set(origin))
        return saved->receipt;
    const auto intent = fetch(origin);
    protocol::validate_notice_set_intent(intent);
    if (intent.reference().SerializeAsString() != origin.SerializeAsString()
        || intent.data_owner().SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "event set source or target differs");
    verify_set_members(intent.payload());
    return save_body(origin, intent.payload().SerializeAsString(), intent.prepared_at_ms());
}

std::vector<intelligence::proto::NoticeStorageReceipt> IntelligenceContent::receipts(
    std::stop_token stop) const
{
    std::vector<intelligence::proto::NoticeStorageReceipt> result;
    if (!authority())
        return result;
    for (const auto& entry : fs::directory_iterator(root_ / "receipts")) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "intelligence content inspection interrupted");
        const auto receipt =
            message<intelligence::proto::NoticeStorageReceipt>(entry.path(), 16384);
        protocol::validate_notice_storage(receipt);
        if (entry.path().filename() != key(receipt.origin()) + ".pb"
            || receipt.content().owner().SerializeAsString() != owner_.SerializeAsString())
            throw std::invalid_argument("intelligence receipt filename or owner identity mismatch");
        result.push_back(receipt);
        if (result.size() > 100000)
            throw Error(
                ErrorCode::resource_exhausted,
                "intelligence receipt catalogue exceeds capacity");
    }
    return result;
}

std::optional<IntelligenceContent::Read> IntelligenceContent::find(
    const resource::proto::Object& origin) const
{
    const auto id = key(origin);
    if (!authority())
        return {};
    const auto file = root_ / "receipts" / (id + ".pb");
    if (!fs::exists(file) && !fs::is_symlink(file))
        return {};
    return read(origin);
}

intelligence::proto::NoticeStorageReceipt
IntelligenceContent::save(const resource::proto::Object& origin, const Fetch& fetch)
{
    if (origin.kind() != "intelligence_preparation")
        throw std::invalid_argument("ordinary notice save requires ordinary preparation");
    if (origin.owner().node_id() != owner_.node_id())
        throw Error(ErrorCode::unavailable, "remote intelligence content transfer unavailable");
    if (const auto saved = find(origin))
        return saved->receipt; // Offline repeat preserves the original save time.
    const auto intent = fetch(origin);
    protocol::validate_notice_intent(intent);
    if (intent.reference().SerializeAsString() != origin.SerializeAsString()
        || intent.data_owner().SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "intelligence preparation source or target mismatch");
    return save_body(origin, intent.payload().SerializeAsString(), intent.prepared_at_ms());
}

intelligence::proto::NoticeStorageReceipt
IntelligenceContent::save_change(const resource::proto::Object& origin, const FetchChange& fetch)
{
    if (origin.kind() != "intelligence_change_preparation")
        throw std::invalid_argument("notice change save requires change preparation");
    if (origin.owner().node_id() != owner_.node_id())
        throw Error(ErrorCode::unavailable, "remote intelligence content transfer unavailable");
    if (const auto saved = find_change(origin))
        return saved->receipt;
    const auto intent = fetch(origin);
    protocol::validate_notice_change_intent(intent);
    if (intent.reference().SerializeAsString() != origin.SerializeAsString()
        || intent.data_owner().SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "notice change source or target mismatch");
    return save_body(origin, intent.payload().SerializeAsString(), intent.prepared_at_ms());
}

intelligence::proto::NoticeStorageReceipt IntelligenceContent::save_body(
    const resource::proto::Object& origin,
    const std::string& body,
    std::int64_t prepared_at)
{
    const auto id = key(origin);
    if (origin.owner().node_id() != owner_.node_id())
        throw Error(ErrorCode::unavailable, "remote intelligence content transfer unavailable");
    const auto file = root_ / "receipts" / (id + ".pb");
    intelligence::proto::NoticeStorageReceipt receipt;
    *receipt.mutable_origin() = origin;
    auto& content = *receipt.mutable_content();
    *content.mutable_owner() = owner_;
    content.set_kind(
        origin.kind() == "intelligence_preparation"           ? "intelligence_content"
            : origin.kind() == "intelligence_set_preparation" ? "intelligence_set_content"
                                                              : "intelligence_change_content");
    content.set_id(origin.version());
    content.set_version(origin.version());
    receipt.set_bytes(body.size());
    receipt.set_stored_at_ms(clock_->utc_now() / 1000000);
    protocol::validate_notice_storage(receipt);
    if (receipt.stored_at_ms() < prepared_at)
        throw Error(ErrorCode::unavailable, "intelligence save clock precedes preparation");
    if (!authority()) {
        create_directories_durably(root_ / "objects");
        create_directories_durably(root_ / "receipts");
        replace_file_durably(
            root_.parent_path() / "intelligence-content.required.pb",
            owner_.SerializeAsString());
    }
    const auto object = root_ / "objects" / origin.version();
    if (fs::exists(object) || fs::is_symlink(object)) {
        if (bytes(object, 1024 * 1024) != body)
            throw std::invalid_argument("retained intelligence content is corrupt");
    } else
        replace_file_durably(object, body);
    replace_file_durably(file, receipt.SerializeAsString());
    return receipt;
}

} // namespace asterion::data
