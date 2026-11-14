#pragma once
#include "store.hpp"
#include <chrono>
#include <functional>
#include <optional>
#include <stop_token>
#include <stdexcept>

namespace asterion::warehouse::acquisition {

using warehouse::CalendarDay;
using warehouse::Contract;
using warehouse::DailyBar;
using warehouse::MinuteBar;
using warehouse::PriceTick;
using warehouse::Settlement;

struct SourceError : std::runtime_error
{
    SourceError(std::string code, std::string message)
        : std::runtime_error(std::move(message)),
          code(std::move(code))
    {}

    std::string code;
};

void check_stop(std::stop_token);
std::chrono::sys_days day_of(std::string_view);
std::string day_text(std::chrono::sys_days);
void require_exchange(std::string_view);
void require_contract(std::string_view);
// The product of an EXCHANGE/product/YYYY-MM identity.
std::string product_of(const std::string& contract);
std::optional<std::string>
contract_of(const std::string& exchange, const std::string& code, const std::string& day);

// Task-local provider state; the service shares the account's request budget.
class Source
{
public:
    virtual ~Source() = default;
    virtual bool supports(std::string_view table, std::string_view exchange) const = 0;

    // Each call returns one whole partition; a source overrides only what it supports.
    virtual std::vector<CalendarDay> calendar(const std::string& exchange, int year);
    virtual std::vector<Contract> contracts(const std::string& exchange);
    virtual std::vector<DailyBar> daily(const std::string& exchange, const std::string& day);
    virtual std::vector<MinuteBar> minutes(const std::string& contract, const std::string& day);
    virtual std::vector<Settlement> settlement(const std::string& exchange, const std::string& day);
    virtual std::vector<PriceTick> price_ticks(const std::string& exchange, const std::string& day);

    std::uint64_t requests() const
    {
        return requests_;
    }

protected:
    std::uint64_t requests_ = 0;
};

using Factory = std::function<std::unique_ptr<Source>(std::stop_token)>;

} // namespace asterion::warehouse::acquisition
