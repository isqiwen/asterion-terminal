#pragma once
#include "manager.hpp"
#include <chrono>
#include <filesystem>
#include <set>
#include <string>

namespace asterion::market_data::acquisition {

// Merged trading days (YYYYMMDD) whose recorded minutes are still to reach
// research. They are kept in `file`, so a restart publishes them again, and a
// day leaves only when a publication that started after its last merge
// succeeded. One publication runs at a
// time; a busy service is tried again each minute, a failed publication each
// hour.
class RecordedPublication
{
public:
    using Clock = std::chrono::steady_clock;

    RecordedPublication(std::filesystem::path file, Manager& manager);
    void add(const std::string& day);
    void poll(Clock::time_point now);

    const std::set<std::string>& pending() const
    {
        return days_;
    }

private:
    void save() const;

    const std::filesystem::path file_;
    Manager& manager_;
    std::set<std::string> days_;
    std::string task_, day_;
    // The day was merged again while its publication ran: publish it again.
    bool merged_again_ = false;
    Clock::time_point next_{};
};

} // namespace asterion::market_data::acquisition
