#pragma once
#include <asterion/protocol/intelligence.hpp>
#include <asterion/foundation/time.hpp>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <vector>

namespace asterion::data {

// Data serial writer, under its warehouse lease. Stores immutable bytes and a
// save receipt; only Intelligence may make a semantic publication decision.
class IntelligenceContent
{
    std::filesystem::path root_;
    resource::proto::Owner owner_;
    std::shared_ptr<const Clock> clock_;
    bool authority() const;
    std::pair<intelligence::proto::NoticeStorageReceipt, std::string> read_body(
        const resource::proto::Object&) const;
    intelligence::proto::NoticeStorageReceipt
    save_body(const resource::proto::Object&, const std::string&, std::int64_t);

public:
    using Fetch =
        std::function<intelligence::proto::NoticePublicationIntent(const resource::proto::Object&)>;
    IntelligenceContent(
        std::filesystem::path,
        resource::proto::Owner,
        std::shared_ptr<const Clock> = std::make_shared<SystemClock>());
    intelligence::proto::NoticeStorageReceipt save(const resource::proto::Object&, const Fetch&);

    struct Read
    {
        intelligence::proto::NoticeStorageReceipt receipt;
        intelligence::proto::NoticePayload payload;
    };

    struct ChangeRead
    {
        intelligence::proto::NoticeStorageReceipt receipt;
        intelligence::proto::NoticeChangePayload payload;
    };

    using FetchChange =
        std::function<intelligence::proto::NoticeChangeIntent(const resource::proto::Object&)>;
    intelligence::proto::NoticeStorageReceipt
    save_change(const resource::proto::Object&, const FetchChange&);
    ChangeRead read_change(const resource::proto::Object&) const;
    std::optional<ChangeRead> find_change(const resource::proto::Object&) const;

    struct SetRead
    {
        intelligence::proto::NoticeStorageReceipt receipt;
        intelligence::proto::NoticeSetPayload payload;
    };

    using FetchSet =
        std::function<intelligence::proto::NoticeSetIntent(const resource::proto::Object&)>;
    intelligence::proto::NoticeStorageReceipt
    save_set(const resource::proto::Object&, const FetchSet&);
    SetRead read_set(const resource::proto::Object&) const;
    std::optional<SetRead> find_set(const resource::proto::Object&) const;
    // Save and stopped cold-cut boundaries verify exact referenced member bytes.
    void verify_set_members(const intelligence::proto::NoticeSetPayload&) const;

    Read read(const resource::proto::Object& origin) const;
    std::optional<Read> find(const resource::proto::Object& origin) const;
    // Metadata only: catalogues do not prove payload integrity or authorize deletion.
    std::vector<intelligence::proto::NoticeStorageReceipt> receipts(std::stop_token = {}) const;
};

} // namespace asterion::data
