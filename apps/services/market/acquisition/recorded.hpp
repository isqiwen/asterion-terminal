#pragma once
#include "source.hpp"
#include <shared_mutex>

namespace asterion::market_data::acquisition {

// Publishes the service's own archive through the same persistent task and
// immutable warehouse as historical providers. No network or provider account.
class Recorded final : public Source
{
public:
    Recorded(
        std::filesystem::path root,
        std::shared_ptr<std::shared_mutex> archive,
        std::stop_token stop);

    bool marks_partial_days() const override
    {
        return true;
    }

    bool supports(std::string_view table, std::string_view exchange) const override;
    MinuteBatch minutes(const std::string& contract, const std::string& day) override;
    std::vector<std::string> minute_contracts(const std::string& day) override;
    // The tick each contract of the day's CTP snapshot stated, for that day alone.
    Rows price_ticks(const std::string& exchange, const std::string& day) override;

private:
    std::filesystem::path root_;
    std::shared_ptr<std::shared_mutex> archive_;
    std::stop_token stop_;
};

} // namespace asterion::market_data::acquisition
