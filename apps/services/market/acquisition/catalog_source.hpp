#pragma once
#include "source.hpp"

namespace asterion::market_data::acquisition {

// The contract catalog Market saved for each trading day: the price tick CTP
// itself stated, valid for that day alone. No network or provider account.
class CatalogSource final : public Source
{
public:
    CatalogSource(std::filesystem::path snapshots, std::stop_token stop);

    // A day's snapshot is complete as soon as the day has one.
    bool marks_partial_days() const override
    {
        return true;
    }

    bool supports(std::string_view table, std::string_view exchange) const override;
    Rows price_ticks(const std::string& exchange, const std::string& day) override;

private:
    std::filesystem::path snapshots_;
    std::stop_token stop_;
};

} // namespace asterion::market_data::acquisition
