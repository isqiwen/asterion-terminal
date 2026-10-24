#pragma once
#include <asterion/protocol/trading_facts.hpp>
#include <filesystem>
#include <optional>
#include <vector>

namespace asterion::trading {

// Immutable payload I/O on the existing account journal writer. Admission and
// publication authority belong to the account journal, never to this directory.
class FactsArchive
{
public:
    FactsArchive(
        std::filesystem::path account_directory,
        resource::proto::Owner,
        std::string account_id,
        protocol::proto::TradingEnvironment);

    struct Inspection
    {
        std::vector<std::filesystem::path> files;
        std::optional<protocol::proto::TradingFactsCapture> capture;
        std::optional<protocol::proto::TradingFactsReceipt> receipt;
        bool object = false;
    };

    // Caller retains the domain's stopped physical lease; no publication or repair.
    Inspection inspect(const resource::proto::Object&) const;
    void stage(const protocol::proto::TradingFactsCapture&) const;
    protocol::proto::TradingFactsCapture prepared(const resource::proto::Object&) const;
    protocol::proto::TradingFactsCapture publish(const resource::proto::Object&) const;
    protocol::proto::TradingFactsCapture read(const resource::proto::Object&) const;
    // Called only after the existing account journal confirms publication.
    void seal(const protocol::proto::TradingFactsReceipt&) const;
    // A direct service read requires the published receipt, not merely a staged object.
    static protocol::proto::TradingFactsStatus
    read_published(const std::filesystem::path& account_directory, const resource::proto::Object&);

private:
    std::filesystem::path root_;
    resource::proto::Owner owner_;
    std::string account_id_;
    protocol::proto::TradingEnvironment environment_;
    void check(const resource::proto::Object&) const;
    protocol::proto::TradingFactsCapture
    load(const std::filesystem::path&, const resource::proto::Object&) const;
};

} // namespace asterion::trading
