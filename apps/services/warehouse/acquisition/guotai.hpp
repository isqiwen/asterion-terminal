#pragma once
#include "exchanges.hpp"

namespace asterion::warehouse::acquisition {

class Guotai final : public Source
{
public:
    Guotai(std::vector<std::string> exchanges, std::stop_token stop, Fetch fetch = exchange_fetch);
    bool supports(std::string_view table, std::string_view exchange) const override;
    std::vector<PriceTick>
    price_ticks(const std::string& exchange, const std::string& day) override;

private:
    std::vector<std::string> exchanges_;
    std::stop_token stop_;
    Fetch fetch_;
    std::string day_;
    std::vector<PriceTick> rows_;
};

} // namespace asterion::warehouse::acquisition
