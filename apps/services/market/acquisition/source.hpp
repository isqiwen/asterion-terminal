#pragma once
#include "warehouse/store.hpp"
#include <chrono>
#include <functional>
#include <optional>
#include <stop_token>
#include <stdexcept>

namespace asterion::market_data::acquisition {

using Rows = std::vector<warehouse::Row>;

struct MinuteBatch
{
    Rows rows, observations;
};

struct SourceError : std::runtime_error
{
    std::string code;

    SourceError(std::string code, std::string message)
        : std::runtime_error(std::move(message)),
          code(std::move(code))
    {}
};

void check_stop(std::stop_token);
std::chrono::sys_days day_of(std::string_view);
std::string day_text(std::chrono::sys_days);
void require_exchange(std::string_view);
void require_contract(std::string_view);
std::optional<std::string>
contract_of(const std::string& exchange, const std::string& code, const std::string& day);

// Task-local provider state; the service shares the account's request budget.
class Source
{
public:
    virtual ~Source() = default;
    virtual bool supports(std::string_view table, std::string_view exchange) const = 0;

    // Whether a day still trading can be taken: only a source that marks what
    // it did not see as partial, so the day is not stored as complete.
    virtual bool marks_partial_days() const
    {
        return false;
    }

    virtual Rows calendar(const std::string& exchange, int year);
    virtual Rows contracts(const std::string& exchange);
    virtual Rows daily(const std::string& exchange, const std::string& day);
    virtual MinuteBatch minutes(const std::string& contract, const std::string& day);
    // Contracts this source holds minutes of on `day`, for a task that names none.
    virtual std::vector<std::string> minute_contracts(const std::string& day);
    virtual Rows settlement(const std::string& exchange, const std::string& day);

    std::uint64_t requests() const
    {
        return requests_;
    }

protected:
    std::uint64_t requests_ = 0;
};

using Factory = std::function<std::unique_ptr<Source>(std::stop_token)>;

} // namespace asterion::market_data::acquisition
