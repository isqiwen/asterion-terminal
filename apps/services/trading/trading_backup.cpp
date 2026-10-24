#include "trading_backup.hpp"
#include "account_journal.hpp"
#include "facts_archive.hpp"
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>
#include <map>
#include <set>

namespace asterion::trading {
namespace fs = std::filesystem;

namespace {

void artifact(const std::string& value)
{
    if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid risk plugin artifact");
}

void cancelled(std::stop_token stop)
{
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "Trading cold backup interrupted");
}

struct FixedDecision
{
    protocol::proto::TradingFactsReceipt receipt;
    std::string policy, artifact;
};

} // namespace

ColdBackup
cold_backup(const fs::path& directory, const resource::proto::Owner& owner, std::stop_token stop)
{
    validate_id(owner.node_id());
    validate_id(owner.service_id());
    if (owner.domain() != "trading" || !directory.is_absolute() || fs::is_symlink(directory)
        || !fs::is_directory(directory) || fs::is_symlink(directory / "facts"))
        throw std::invalid_argument("Trading cold backup requires its owned root");
    cancelled(stop);
    const auto root = fs::canonical(directory);
    ColdBackup result;
    auto& snapshot = result.snapshot;
    snapshot.owner = owner;
    snapshot.directory = root;
    snapshot.ownership = std::make_unique<FileLock>(
        root / "facts",
        AccountJournal::ownership_file,
        FileLock::Access::shared_existing);
    std::set<fs::path> declared;
    const auto add = [&](const fs::path& relative, const std::string& format) {
        cancelled(stop);
        const auto file = root / relative;
        if (fs::is_symlink(file.parent_path()) || fs::is_symlink(file)
            || !fs::is_regular_file(file))
            throw std::invalid_argument("Trading backup evidence is missing");
        if (format == "coordination" && fs::file_size(file))
            throw std::invalid_argument("Trading coordination slot is not empty");
        if (declared.insert(relative).second)
            snapshot.files.push_back({file, relative, format});
    };
    std::set<std::string> limitations;
    std::set<std::string> risks;
    std::map<std::string, std::string> revisions;
    std::map<std::string, FixedDecision> facts;
    Json manifest;
    std::string policy, risk;
    const auto files = AccountJournal::inspect_backup(
        root,
        [&](const Json& header) {
            manifest = account_manifest(header);
            policy = header.at("policy_revision").get<std::string>();
            risk = header.at("risk_artifact").get<std::string>();
            artifact(risk);
            revisions.emplace(policy, risk);
            risks.insert(risk);
        },
        [&](const JournalRecord& record) {
            cancelled(stop);
            if (const auto* command = std::get_if<CommandRecord>(&record)) {
                if (command->policy_revision != policy)
                    throw std::invalid_argument(
                        "trading command policy does not match the active revision");
                if (command->request.as<ChangePolicy>()) {
                    artifact(command->risk_artifact);
                    if (!revisions.emplace(command->new_policy_revision, command->risk_artifact)
                            .second)
                        throw std::invalid_argument("account policy revision is repeated");
                    policy = command->new_policy_revision;
                    risk = command->risk_artifact;
                    risks.insert(risk);
                }
            }
            if (const auto* decision = std::get_if<FactsDecision>(&record)) {
                const auto& receipt = decision->receipt;
                if (receipt.reference().owner().SerializeAsString() != owner.SerializeAsString())
                    throw Error(ErrorCode::conflict, "Trading backup fixed facts owner mismatch");
                if (receipt.state() == protocol::proto::TradingFactsReceipt::ADMITTED) {
                    if (facts.size() >= 100000)
                        throw Error(
                            ErrorCode::resource_exhausted,
                            "Trading backup facts count exceeds capacity");
                    facts.emplace(receipt.reference().id(), FixedDecision{receipt, policy, risk});
                } else {
                    // AccountJournal already proved this publication's preceding exact admission.
                    facts.at(receipt.reference().id()).receipt = receipt;
                }
            }
        },
        stop);
    for (const auto& file : files)
        add(file, "sqlite");
    add(fs::path("facts") / AccountJournal::ownership_file, "coordination");
    const auto account = manifest.at("account_id").get<std::string>();
    snapshot.scope = {
        {"account_id", account},
        {"environment", manifest.at("environment").get<std::string>()}
    };
    const auto environment = manifest.at("environment") == "real"
        ? protocol::proto::REAL
        : protocol::proto::BROKER_SIMULATION;
    FactsArchive archive(root, owner, account, environment);
    for (const auto& [id, decision] : facts) {
        cancelled(stop);
        const auto& receipt = decision.receipt;
        const auto inspected = archive.inspect(receipt.reference());
        if (inspected.capture) {
            const auto& fixed = inspected.capture->facts();
            if (fixed.policy_revision() != decision.policy
                || fixed.risk_artifact() != decision.artifact)
                throw Error(
                    ErrorCode::conflict,
                    "Trading backup facts policy differs from its admission");
        }
        if (receipt.state() == protocol::proto::TradingFactsReceipt::PUBLISHED) {
            if (!inspected.object)
                throw Error(
                    ErrorCode::not_found,
                    "Trading backup published facts payload is missing");
            if (inspected.receipt) {
                if (inspected.receipt->SerializeAsString() != receipt.SerializeAsString())
                    throw Error(
                        ErrorCode::conflict,
                        "Trading backup facts receipt differs from its journal");
                result.objects.push_back(receipt.reference());
            } else
                limitations.insert("published_facts_receipt_missing");
        } else {
            if (inspected.receipt)
                throw Error(
                    ErrorCode::conflict,
                    "Trading publication receipt has no published journal decision");
            if (!inspected.capture)
                limitations.insert("admitted_facts_payload_missing");
        }
        for (const auto& file : inspected.files)
            add(file, "protobuf");
    }
    // Retain every captured risk programme, including a prepared but unadopted policy.
    std::set<std::string> programs;
    if (fs::exists(root / "plugins")) {
        if (fs::is_symlink(root / "plugins") || !fs::is_directory(root / "plugins"))
            throw std::invalid_argument("Trading backup plugin directory is invalid");
        for (const auto& entry : fs::directory_iterator(root / "plugins")) {
            cancelled(stop);
            const auto digest = entry.path().filename().string();
            artifact(digest);
            if (entry.is_symlink() || !entry.is_directory() || programs.size() >= 100000)
                throw std::invalid_argument("Trading backup plugin entry is invalid");
            fs::path file;
            for (const auto* name : {"risk-plugin.so", "risk-plugin.dylib"})
                if (fs::exists(entry.path() / name) || fs::is_symlink(entry.path() / name)) {
                    if (!file.empty())
                        throw std::invalid_argument("Trading backup plugin platform is ambiguous");
                    file = entry.path() / name;
                }
            if (file.empty())
                throw std::invalid_argument("Trading backup captured risk programme is missing");
            if (fs::is_symlink(file) || !fs::is_regular_file(file) || !fs::file_size(file)
                || fs::file_size(file) > max_artifact_bytes || sha256_file(file) != digest)
                throw std::invalid_argument("risk plugin artifact does not match its owner");
            const auto platform = artifact_platform(file);
            if (!((platform.os == "linux" && platform.arch == "x86_64")
                    || (platform.os == "macos" && platform.arch == "arm64")))
                throw std::invalid_argument("unsupported archived program platform");
            programs.insert(digest);
            add(file.lexically_relative(root), "binary");
            resource::proto::Object object;
            *object.mutable_owner() = owner;
            object.set_kind("trading_risk_program");
            object.set_id(digest);
            object.set_version(digest);
            result.objects.push_back(std::move(object));
        }
    }
    for (const auto& required : risks) {
        resource::proto::Object object;
        *object.mutable_owner() = owner;
        object.set_kind("trading_risk_program");
        object.set_id(required);
        object.set_version(required);
        snapshot.dependencies.push_back({std::move(object), programs.contains(required)});
    }
    limitations.insert("trading_original_host_program_identity_not_recorded");
    limitations.insert("trading_header_original_owner_not_recorded");
    limitations.insert("available_journal_is_not_complete_broker_history");
    snapshot.limitations.assign(limitations.begin(), limitations.end());
    snapshot.exclusions = {
        "ctp_runtime_cache",
        "credentials",
        "active_authorization",
        "global_account_execution_binding",
        "process_and_lease_state"
    };
    for (fs::recursive_directory_iterator entry(root), end; entry != end; ++entry) {
        cancelled(stop);
        const auto relative = entry->path().lexically_relative(root);
        if (*relative.begin() == "ctp-flow") {
            if (entry->is_directory())
                entry.disable_recursion_pending();
            continue;
        }
        if (entry->is_symlink())
            throw std::invalid_argument("Trading backup rejects symbolic links");
        if (entry->is_directory()) {
            snapshot.directories.push_back(relative);
            continue;
        }
        if (!entry->is_regular_file())
            throw std::invalid_argument("Trading backup rejects unsupported file types");
        if (declared.contains(relative))
            continue;
        if (relative == "journal.sqlite-shm")
            continue;
        if ((relative == "journal.sqlite-wal" || relative == "journal.sqlite-journal")
            && !entry->file_size())
            continue;
        throw std::invalid_argument("undeclared Trading evidence; owner inspection required");
    }
    return result;
}

} // namespace asterion::trading
