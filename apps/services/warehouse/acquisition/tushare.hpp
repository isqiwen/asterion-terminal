#pragma once
#include "exchange_rules.hpp"
#include "source.hpp"
#include <nlohmann/json.hpp>
#include <set>

namespace asterion::warehouse::acquisition {

// Production transport has one fixed HTTPS origin. Tests inject a scripted
// transport; deployment configuration cannot redirect credentials elsewhere.
using Post = std::function<std::string(const std::string&, std::stop_token)>;
std::string tushare_post(const std::string& body, std::stop_token);

class Tushare final : public Source
{
public:
    Tushare(
        std::string token,
        std::shared_ptr<Budget>,
        std::stop_token,
        Post = tushare_post,
        DceFeeRules dce_fees = {});
    bool supports(std::string_view table, std::string_view exchange) const override;
    std::vector<CalendarDay> calendar(const std::string&, int year) override;
    std::vector<Contract> contracts(const std::string&) override;
    std::vector<DailyBar> daily(const std::string&, const std::string&) override;
    std::vector<MinuteBar> minutes(const std::string&, const std::string&) override;
    std::vector<Settlement> settlement(const std::string&, const std::string&) override;
    std::vector<WarehouseReceipt>
    warehouse_receipts(const std::string&, const std::string&) override;
    std::vector<MemberPosition> member_positions(const std::string&, const std::string&) override;
    std::vector<Inventory> inventory(const std::string&, const std::string&) override;

private:
    struct Table;

    struct Window
    {
        std::string contract, first, last;
        std::map<std::string, std::vector<MinuteBar>> rows;
    };

    struct LimitWindow
    {
        std::string first, last;
        std::map<std::string, std::map<std::string, std::pair<Cell, Cell>>> by_day;
    };

    Table ask(
        const std::string& api,
        const nlohmann::json& parameters,
        const std::string& fields,
        std::size_t limit);
    std::set<std::string> products(const std::string& exchange, const std::string& day);
    const std::map<std::string, std::pair<Cell, Cell>>& daily_limits(const std::string& day);
    const CalendarDay& calendar_day(const std::string& exchange, const std::string& day);
    std::string trading_day(const std::string& exchange, std::chrono::sys_seconds end);

    std::string token_;
    std::shared_ptr<Budget> budget_;
    std::stop_token stop_;
    Post post_;
    std::map<std::string, std::vector<Contract>> contracts_;
    std::map<std::pair<std::string, int>, std::map<std::string, CalendarDay>> calendars_;
    Window window_;
    DceFeeRules dce_fees_;
    LimitWindow limits_;
    bool limits_permitted_ = true;
};

} // namespace asterion::warehouse::acquisition
