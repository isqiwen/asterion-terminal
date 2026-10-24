#include "facts_archive.hpp"
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <fstream>

namespace asterion::trading {
namespace fs = std::filesystem;

FactsArchive::FactsArchive(
    fs::path directory,
    resource::proto::Owner owner,
    std::string account_id,
    protocol::proto::TradingEnvironment environment)
    : root_(std::move(directory) / "facts"),
      owner_(std::move(owner)),
      account_id_(std::move(account_id)),
      environment_(environment)
{
    validate_id(owner_.node_id());
    validate_id(owner_.service_id());
    validate_id(account_id_);
    if (!root_.is_absolute() || fs::is_symlink(root_) || owner_.domain() != "trading"
        || (environment_ != protocol::proto::REAL
            && environment_ != protocol::proto::BROKER_SIMULATION))
        throw std::invalid_argument("invalid trading facts archive binding");
}

void FactsArchive::check(const resource::proto::Object& ref) const
{
    protocol::validate_trading_facts_reference(ref);
    if (ref.owner().SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "trading facts owner mismatch");
}

protocol::proto::TradingFactsCapture
FactsArchive::load(const fs::path& path, const resource::proto::Object& ref) const
{
    check(ref);
    if (fs::is_symlink(root_) || fs::is_symlink(path.parent_path())
        || !fs::is_directory(path.parent_path()) || fs::is_symlink(path)
        || !fs::is_regular_file(path))
        throw Error(ErrorCode::not_found, "fixed trading facts payload is missing");
    if (fs::file_size(path) > protocol::max_trading_facts_bytes)
        throw std::invalid_argument("invalid trading facts payload size");
    std::ifstream input(path, std::ios::binary);
    protocol::proto::TradingFactsCapture result;
    if (!result.ParseFromIstream(&input) || input.bad())
        throw std::invalid_argument("invalid trading facts protobuf");
    protocol::validate_trading_facts(result);
    if (result.reference().SerializeAsString() != ref.SerializeAsString()
        || result.facts().account_id() != account_id_
        || result.facts().environment() != environment_)
        throw Error(ErrorCode::conflict, "trading facts binding mismatch");
    return result;
}

void FactsArchive::stage(const protocol::proto::TradingFactsCapture& capture) const
{
    protocol::validate_trading_facts(capture);
    const auto& ref = capture.reference();
    check(ref);
    if (capture.facts().account_id() != account_id_
        || capture.facts().environment() != environment_)
        throw Error(ErrorCode::conflict, "trading facts binding mismatch");
    const auto object = root_ / "objects" / (ref.version() + ".pb");
    const auto stage = root_ / "staging" / (ref.id() + ".pb");
    if (fs::exists(object) || fs::is_symlink(object)) {
        (void)load(object, ref);
        sync_file_durably(object);
        return;
    }
    if (fs::exists(stage) || fs::is_symlink(stage)) {
        (void)load(stage, ref);
        sync_file_durably(stage);
        return;
    }
    create_directories_durably(stage.parent_path());
    replace_file_durably(stage, capture.SerializeAsString());
}

protocol::proto::TradingFactsCapture FactsArchive::prepared(
    const resource::proto::Object& ref) const
{
    check(ref);
    const auto object = root_ / "objects" / (ref.version() + ".pb");
    if (fs::exists(object) || fs::is_symlink(object))
        return load(object, ref);
    return load(root_ / "staging" / (ref.id() + ".pb"), ref);
}

protocol::proto::TradingFactsCapture FactsArchive::publish(const resource::proto::Object& ref) const
{
    auto result = prepared(ref);
    const auto object = root_ / "objects" / (ref.version() + ".pb");
    if (fs::exists(object))
        sync_file_durably(object);
    else {
        create_directories_durably(object.parent_path());
        publish_file_durably(root_ / "staging" / (ref.id() + ".pb"), object);
    }
    return result;
}

protocol::proto::TradingFactsCapture FactsArchive::read(const resource::proto::Object& ref) const
{
    check(ref);
    return load(root_ / "objects" / (ref.version() + ".pb"), ref);
}

namespace {

protocol::proto::TradingFactsReceipt published_receipt(const fs::path& file)
{
    if (fs::is_symlink(file.parent_path()) || !fs::is_directory(file.parent_path())
        || fs::is_symlink(file) || !fs::is_regular_file(file))
        throw Error(ErrorCode::not_found, "trading facts publication receipt is missing");
    if (fs::file_size(file) > 4096)
        throw std::invalid_argument("invalid trading facts receipt size");
    std::ifstream input(file, std::ios::binary);
    protocol::proto::TradingFactsReceipt receipt;
    if (!receipt.ParseFromIstream(&input) || input.bad())
        throw std::invalid_argument("invalid trading facts receipt protobuf");
    protocol::validate_trading_facts_receipt(receipt);
    if (receipt.state() != protocol::proto::TradingFactsReceipt::PUBLISHED)
        throw std::invalid_argument("trading facts receipt is not published");
    return receipt;
}

} // namespace

FactsArchive::Inspection FactsArchive::inspect(const resource::proto::Object& ref) const
{
    check(ref);
    Inspection result;
    for (const auto* part : {"staging", "objects"}) {
        const bool object = std::string_view(part) == "objects";
        const auto relative =
            fs::path("facts") / part / ((object ? ref.version() : ref.id()) + ".pb");
        const auto file = root_.parent_path() / relative;
        if (fs::exists(file) || fs::is_symlink(file)) {
            result.capture = load(file, ref);
            result.files.push_back(relative);
            result.object = result.object || object;
        }
    }
    const auto relative = fs::path("facts/published") / (ref.id() + ".pb");
    const auto file = root_.parent_path() / relative;
    if (fs::exists(file) || fs::is_symlink(file)) {
        result.receipt = published_receipt(file);
        if (result.receipt->reference().SerializeAsString() != ref.SerializeAsString())
            throw Error(ErrorCode::conflict, "trading facts publication identity mismatch");
        result.files.push_back(relative);
    }
    return result;
}

void FactsArchive::seal(const protocol::proto::TradingFactsReceipt& receipt) const
{
    protocol::validate_trading_facts_receipt(receipt);
    if (receipt.state() != protocol::proto::TradingFactsReceipt::PUBLISHED)
        throw std::invalid_argument("trading facts are not published");
    (void)read(receipt.reference());
    const auto file = root_ / "published" / (receipt.reference().id() + ".pb");
    if (fs::exists(file) || fs::is_symlink(file)) {
        if (published_receipt(file).SerializeAsString() != receipt.SerializeAsString())
            throw Error(ErrorCode::conflict, "trading facts publication receipt changed");
        sync_file_durably(file);
    } else {
        create_directories_durably(file.parent_path());
        replace_file_durably(file, receipt.SerializeAsString());
    }
}

protocol::proto::TradingFactsStatus
FactsArchive::read_published(const fs::path& directory, const resource::proto::Object& ref)
{
    protocol::validate_trading_facts_reference(ref);
    const auto root = directory / "facts";
    if (!directory.is_absolute() || fs::is_symlink(root) || !fs::is_directory(root))
        throw Error(ErrorCode::not_found, "trading facts archive is missing");
    const auto receipt = published_receipt(root / "published" / (ref.id() + ".pb"));
    if (receipt.reference().SerializeAsString() != ref.SerializeAsString())
        throw Error(ErrorCode::conflict, "trading facts publication identity mismatch");
    const auto file = root / "objects" / (ref.version() + ".pb");
    if (fs::is_symlink(file.parent_path()) || !fs::is_directory(file.parent_path())
        || fs::is_symlink(file) || !fs::is_regular_file(file))
        throw Error(ErrorCode::not_found, "trading facts payload is missing");
    if (fs::file_size(file) > protocol::max_trading_facts_bytes)
        throw std::invalid_argument("invalid trading facts payload size");
    std::ifstream input(file, std::ios::binary);
    protocol::proto::TradingFactsStatus result;
    *result.mutable_operation() = receipt;
    if (!result.mutable_capture()->ParseFromIstream(&input) || input.bad())
        throw std::invalid_argument("invalid trading facts payload protobuf");
    (void)protocol::decode_trading_facts_status(result);
    return result;
}

} // namespace asterion::trading
