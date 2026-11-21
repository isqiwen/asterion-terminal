#pragma once
#include "store.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>

namespace asterion::warehouse::acquisition {

using warehouse::CalendarDay;
using warehouse::Contract;
using warehouse::DailyBar;
using warehouse::MinuteBar;
using warehouse::PriceTick;
using warehouse::Settlement;
using warehouse::WarehouseReceipt;
using warehouse::MemberPosition;
using warehouse::Inventory;

struct SourceError : std::runtime_error
{
    SourceError(std::string code, std::string message)
        : std::runtime_error(std::move(message)),
          code(std::move(code))
    {}

    std::string code;
};

void check_stop(std::stop_token);
std::int64_t observation_time();
std::chrono::sys_days day_of(std::string_view);
std::string day_text(std::chrono::sys_days);
void require_exchange(std::string_view);
void require_contract(std::string_view);
// The product of an EXCHANGE/product/YYYY-MM identity.
std::string product_of(const std::string& contract);
std::optional<std::string>
contract_of(const std::string& exchange, const std::string& code, const std::string& day);

// Shared sliding-window request limit across tasks for one provider.
class Budget
{
public:
    explicit Budget(unsigned per_minute);
    void acquire(std::stop_token);

private:
    unsigned capacity_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<std::chrono::steady_clock::time_point> sent_;
};

// Task-local provider state; the service shares the provider's request budget.
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
    virtual std::vector<WarehouseReceipt>
    warehouse_receipts(const std::string&, const std::string&);
    virtual std::vector<MemberPosition> member_positions(const std::string&, const std::string&);
    virtual std::vector<Inventory> inventory(const std::string&, const std::string&);

    std::uint64_t requests() const
    {
        return requests_;
    }

protected:
    std::uint64_t requests_ = 0;
};

using Factory = std::function<std::unique_ptr<Source>(std::stop_token)>;

} // namespace asterion::warehouse::acquisition
