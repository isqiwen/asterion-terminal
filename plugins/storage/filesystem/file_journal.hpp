#pragma once
#include <asterion/kernel/journal_port.hpp>
#include <filesystem>
namespace asterion {
class FileJournal final : public JournalPort {
public:
    explicit FileJournal(std::filesystem::path directory);
    ~FileJournal() override;
    PluginDescriptor descriptor() const override;
    void start() override;
    void stop() noexcept override;
    std::vector<Json> read() const override;
    void append(const Json& record) override;
private:
    std::filesystem::path directory_;
    std::intptr_t handle_ = -1;
    bool poisoned_ = false;
    std::size_t count_ = 0;
    std::uintmax_t bytes_ = 0;
};
}
