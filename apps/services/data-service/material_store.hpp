#pragma once
#include <asterion/protocol/material.hpp>
#include <filesystem>
#include <stop_token>

namespace asterion::data {

// Data's current serial writer owns publication, under the existing warehouse lock.
// Acquisition scheduling/cancellation/recovery belong to its existing TaskStore.
// This library owns original bytes and acquisition receipts, not a second task queue.
class Materials
{
public:
    Materials(std::filesystem::path directory, resource::proto::Owner owner);
    std::optional<proto::MaterialReceipt> find(const std::string& id) const;

    class Prepared
    {
        friend class Materials;
        proto::MaterialReceipt receipt_;
        std::string bytes_;

        Prepared(proto::MaterialReceipt receipt, std::string bytes)
            : receipt_(std::move(receipt)),
              bytes_(std::move(bytes))
        {}

    public:
        const proto::MaterialReceipt& receipt() const noexcept
        {
            return receipt_;
        }
    };

    Prepared prepare(
        const proto::MaterialRequest&,
        std::string bytes,
        const std::string& media_type,
        std::int64_t acquired_at_ms,
        std::optional<std::int64_t> source_published_at_ms = {}) const;
    proto::MaterialReceipt publish(const Prepared&);

    struct Read
    {
        proto::MaterialReceipt receipt;
        std::string bytes;
        proto::MaterialText text;
    };

    Read
    read(const std::string& id, const std::optional<resource::proto::Object>& expected = {}) const;
    std::vector<proto::MaterialReceipt> receipts(std::stop_token stop = {}) const;

private:
    bool authority() const;
    std::filesystem::path receipt_file(const std::string&) const;
    std::filesystem::path root_;
    resource::proto::Owner owner_;
};

} // namespace asterion::data
