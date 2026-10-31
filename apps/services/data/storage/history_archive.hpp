#pragma once
#include <asterion/futures/history_store.hpp>
#include <asterion/data.pb.h>
#include <filesystem>
#include <optional>
#include <mutex>
#include <stop_token>

namespace asterion::history_files {

class Archive final
{
public:
    enum class Access
    {
        writer,
        read_only,
        cold_read_only // Caller holds the stopped domain ownership lock.
    };
    explicit Archive(std::filesystem::path root, Access access = Access::writer);

    void start() {}

    void stop() noexcept {}

    std::vector<HistoryDataset> datasets(const HistoryFilter&) const;
    std::filesystem::path directory(
        const HistoryIdentity&,
        const std::string& source,
        unsigned interval,
        const std::string& acquisition) const;
    // Persisted absolute paths are local locators, never cross-node identity.
    // Resolve their validated acquisition suffix under this owner's current root.
    std::filesystem::path
    resolve_directory(const data::proto::MinuteDownload&, const std::string& locator) const;
    // The data owner verifies files on its file pool before retaining prepared
    // evidence. Publication only exposes that immutable, already verified record.
    void publish_verified(const data::proto::HistoryRecord&);
    data::proto::HistoryRecord get(const std::string& id) const;
    std::optional<data::proto::HistoryRetirement> retirement(const std::string& id) const;
    std::vector<data::proto::HistoryRetirement>
    retirements(const std::string& after, unsigned limit) const;
    std::uint64_t
    retirement_payload_bytes(const data::proto::HistoryRecord&, std::stop_token) const;
    void begin_retirement(const data::proto::HistoryRecord&, const std::string& manifest);
    void clean_retirement(const std::string& id, std::stop_token);
    void finish_retirement(const std::string& id);
    void save_named_dataset(const data::proto::NamedDataset&);
    data::proto::NamedDataset named_dataset(const std::string&) const;
    bool has_named_dataset(const std::string&) const;
    data::proto::NamedDatasets named_datasets() const;

private:
    std::filesystem::path root_;
    Access access_;
    mutable std::recursive_mutex retirement_mutex_;
    void writable() const;
    void check_retirement_authority() const;
    std::filesystem::path retirement_directory(const data::proto::HistoryRecord&) const;
};

} // namespace asterion::history_files
