#pragma once
#include "exchanges.hpp"
#include "tushare.hpp"
#include <map>

namespace asterion::warehouse::acquisition {

// Sina Finance's public futures quotes, the last source to fill a gap. It answers
// by contract, so a day's daily bars need Tushare's contract table: without it the
// day would look whole while missing the contracts that expired since.
// Its minutes reach back about two trading days. CFFEX is not on this service.
class Sina final : public Source
{
public:
    Sina(
        std::vector<std::string> exchanges,
        std::filesystem::path data,
        std::shared_ptr<Budget> budget,
        std::stop_token,
        Fetch = exchange_fetch);
    bool supports(std::string_view table, std::string_view exchange) const override;
    std::vector<DailyBar> daily(const std::string& exchange, const std::string& day) override;
    std::vector<MinuteBar> minutes(const std::string& contract, const std::string& day) override;

private:
    std::string
    get(const std::string& service, const std::string& contract, const std::string& suffix = {});

    std::vector<std::string> exchanges_;
    std::filesystem::path data_;
    std::shared_ptr<Budget> budget_;
    std::stop_token stop_;
    Fetch fetch_;
    // By contract, for this task: each answer holds every day.
    std::map<std::string, std::map<std::string, DailyBar>> daily_;
    std::map<std::string, std::map<std::string, std::vector<MinuteBar>>> minutes_;
    std::map<std::string, std::vector<Contract>> listed_;
};

} // namespace asterion::warehouse::acquisition
