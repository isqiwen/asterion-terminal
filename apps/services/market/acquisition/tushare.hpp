#pragma once
#include "source.hpp"
#include <condition_variable>
#include <deque>
#include <nlohmann/json.hpp>

namespace asterion::market_data::acquisition {

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

// Production transport has one fixed HTTPS origin. Tests inject a scripted
// transport; deployment configuration cannot redirect credentials elsewhere.
using Post = std::function<std::string(const std::string&, std::stop_token)>;
std::string tushare_post(const std::string& body, std::stop_token);

class Tushare final : public Source
{
public:
    Tushare(std::string token, std::shared_ptr<Budget>, std::stop_token, Post = tushare_post);
    bool supports(std::string_view table, std::string_view exchange) const override;
    Rows calendar(const std::string&, int year) override;
    Rows contracts(const std::string&) override;
    Rows daily(const std::string&, const std::string&) override;
    MinuteBatch minutes(const std::string&, const std::string&) override;

private:
    struct Table;
    Table ask(
        const std::string& api,
        const nlohmann::json& parameters,
        const std::string& fields,
        std::size_t limit);
    std::string token_;
    std::shared_ptr<Budget> budget_;
    std::stop_token stop_;
    Post post_;
    std::map<std::string, Rows> contracts_;
    std::map<std::pair<std::string, int>, std::map<std::string, warehouse::Row>> calendars_;
    warehouse::Row calendar_day(const std::string& exchange, const std::string& day);
    std::string trading_day(const std::string& exchange, std::chrono::sys_seconds end);

    struct Window
    {
        std::string contract, first, last;
        std::map<std::string, Rows> rows;
    } window_;
};

} // namespace asterion::market_data::acquisition
