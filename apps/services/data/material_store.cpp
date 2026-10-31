#include "material_store.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/protocol/trading.hpp>
#include <fstream>
#include <algorithm>

namespace asterion::data {
namespace fs = std::filesystem;

namespace {

void directory(const fs::path& path)
{
    if (fs::is_symlink(path) || !fs::is_directory(path))
        throw std::invalid_argument("material directory is unavailable");
}

std::string bytes(const fs::path& path, std::uintmax_t limit)
{
    directory(path.parent_path());
    if (fs::is_symlink(path) || !fs::is_regular_file(path) || fs::file_size(path) > limit)
        throw std::invalid_argument("material evidence is missing or invalid");
    std::ifstream input(path, std::ios::binary);
    std::string result{std::istreambuf_iterator<char>(input), {}};
    if (input.bad())
        throw std::runtime_error("material read failed");
    return result;
}

template <class T>
T message(const fs::path& path)
{
    T result;
    if (!result.ParseFromString(bytes(path, 16384)))
        throw std::invalid_argument("invalid material metadata");
    protocol::validate_message(result);
    return result;
}

} // namespace

Materials::Materials(fs::path directory, resource::proto::Owner owner)
    : root_(std::move(directory) / "materials"),
      owner_(std::move(owner))
{
    validate_id(owner_.node_id());
    validate_id(owner_.service_id());
    if (!root_.is_absolute() || owner_.domain() != "data")
        throw std::invalid_argument("invalid material owner or root");
    (void)authority();
}

bool Materials::authority() const
{
    directory(root_.parent_path());
    const auto guard = root_.parent_path() / "materials.required.pb";
    if (!fs::exists(guard) && !fs::is_symlink(guard)) {
        if (fs::exists(root_) || fs::is_symlink(root_)) {
            directory(root_);
            // Resume only an empty initial directory preparation. Lost receipts/objects
            // never establish a new owner merely because the guard disappeared.
            for (const auto& e : fs::directory_iterator(root_)) {
                const auto name = e.path().filename().string();
                if ((name != "objects" && name != "receipts") || e.is_symlink() || !e.is_directory()
                    || !fs::is_empty(e.path()))
                    throw std::invalid_argument("material ownership authority is missing");
            }
        }
        return false;
    }
    const auto saved = message<resource::proto::Owner>(guard);
    if (saved.SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "material owner mismatch");
    directory(root_);
    directory(root_ / "objects");
    directory(root_ / "receipts");
    return true;
}

fs::path Materials::receipt_file(const std::string& id) const
{
    validate_id(id);
    return root_ / "receipts" / (id + ".pb");
}

std::optional<proto::MaterialReceipt> Materials::find(const std::string& id) const
{
    const auto file = receipt_file(id);
    if (!authority() || (!fs::exists(file) && !fs::is_symlink(file)))
        return {};
    auto result = message<proto::MaterialReceipt>(file);
    protocol::validate_material_receipt(result);
    if (result.reference().id() != id
        || result.reference().owner().SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "material receipt owner or identity mismatch");
    return result;
}

Materials::Prepared Materials::prepare(
    const proto::MaterialRequest& request,
    std::string body,
    const std::string& media_type,
    std::int64_t acquired_at_ms,
    std::optional<std::int64_t> source_published_at_ms) const
{
    auto receipt = protocol::freeze_material(
        owner_,
        request,
        body,
        media_type,
        acquired_at_ms,
        source_published_at_ms);
    if (const auto previous = find(request.id())) {
        const auto& a = previous->evidence();
        const auto& b = receipt.evidence();
        if (a.request().SerializeAsString() != request.SerializeAsString()
            || a.content().version() != b.content().version() || a.media_type() != b.media_type()
            || a.has_source_published_at_ms() != b.has_source_published_at_ms()
            || a.source_published_at_ms() != b.source_published_at_ms())
            throw Error(ErrorCode::conflict, "material acquisition identity has different content");
        receipt = *previous; // A retry does not rewrite the first actual acquisition time.
    }
    return Prepared(std::move(receipt), std::move(body));
}

proto::MaterialReceipt Materials::publish(const Prepared& prepared)
{
    const auto& receipt = prepared.receipt_;
    if (receipt.reference().owner().SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "material publication owner mismatch");
    const auto file = receipt_file(receipt.reference().id());
    if (const auto previous = find(receipt.reference().id())) {
        if (previous->SerializeAsString() != receipt.SerializeAsString())
            throw Error(ErrorCode::conflict, "material acquisition already published differently");
        return *previous;
    }
    if (!authority()) {
        create_directories_durably(root_ / "objects");
        create_directories_durably(root_ / "receipts");
        replace_file_durably(
            root_.parent_path() / "materials.required.pb",
            owner_.SerializeAsString());
    }
    const auto object = root_ / "objects" / receipt.evidence().content().version();
    if (fs::exists(object) || fs::is_symlink(object)) {
        if (sha256_bytes(bytes(object, protocol::max_material_bytes))
            != receipt.evidence().content().version())
            throw std::invalid_argument("retained material original is corrupt");
    } else
        replace_file_durably(object, prepared.bytes_);
    replace_file_durably(file, receipt.SerializeAsString());
    return receipt;
}

Materials::Read
Materials::read(const std::string& id, const std::optional<resource::proto::Object>& expected) const
{
    const auto receipt = find(id);
    if (!receipt)
        throw Error(ErrorCode::not_found, "unknown material acquisition");
    if (expected && expected->SerializeAsString() != receipt->reference().SerializeAsString())
        throw Error(ErrorCode::conflict, "material acquisition differs from the fixed reference");
    Read result;
    result.receipt = *receipt;
    result.bytes = bytes(
        root_ / "objects" / receipt->evidence().content().version(),
        protocol::max_material_bytes);
    result.text = protocol::read_material_text(*receipt, result.bytes);
    return result;
}

std::vector<proto::MaterialReceipt> Materials::receipts(std::stop_token stop) const
{
    std::vector<proto::MaterialReceipt> result;
    if (!authority())
        return result;
    for (const auto& entry : fs::directory_iterator(root_ / "receipts")) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "material catalogue cancelled");
        if (entry.is_symlink() || !entry.is_regular_file() || entry.path().extension() != ".pb")
            throw std::invalid_argument("unexpected material receipt entry");
        result.push_back(*find(entry.path().stem().string()));
        if (result.size() > 100000)
            throw Error(ErrorCode::resource_exhausted, "material catalogue exceeds capacity");
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.reference().id() < b.reference().id();
    });
    return result;
}

} // namespace asterion::data
