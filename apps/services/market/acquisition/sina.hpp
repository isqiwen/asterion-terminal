#pragma once
#include "exchanges.hpp"
#include "tushare.hpp"
#include <map>

namespace asterion::market_data::acquisition {

// Sina Finance's public futures quotes, the last source to fill a gap. It answers
// by contract, so a day's daily bars need that day's contract snapshot: without
// it the day would look whole while missing the contracts that expired since.
// Its minutes reach back about two trading days. CFFEX is not on this service.
class Sina final : public Source
{
public:
    Sina(
        std::vector<std::string> exchanges,
        std::filesystem::path snapshots,
        std::shared_ptr<Budget> budget,
        std::stop_token,
        Fetch = exchange_fetch);
    bool supports(std::string_view table, std::string_view exchange) const override;
    Rows daily(const std::string& exchange, const std::string& day) override;
    Rows minutes(const std::string& contract, const std::string& day) override;

private:
    std::string
    get(const std::string& service, const std::string& contract, const std::string& suffix = {});

    std::vector<std::string> exchanges_;
    std::filesystem::path snapshots_;
    std::shared_ptr<Budget> budget_;
    std::stop_token stop_;
    Fetch fetch_;
    // By contract, for this task: each answer holds every day.
    std::map<std::string, std::map<std::string, warehouse::Row>> daily_;
    std::map<std::string, std::map<std::string, Rows>> minutes_;
};

} // namespace asterion::market_data::acquisition
