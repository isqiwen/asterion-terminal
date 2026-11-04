#include "recorded_publication.hpp"
#include <asterion/base/error.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/base/serialization.hpp>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/log.hpp>
#include <fstream>
#include <regex>

namespace asterion::market_data::acquisition {

using namespace std::chrono_literals;

RecordedPublication::RecordedPublication(std::filesystem::path file, Manager& manager)
    : file_(std::move(file)),
      manager_(manager)
{
    if (!std::filesystem::exists(file_))
        return;
    std::ifstream input(file_, std::ios::binary);
    const auto document = parse_json(std::string{std::istreambuf_iterator<char>(input), {}}, 65536);
    for (const auto& day : document) {
        const auto text = day.get<std::string>();
        if (!std::regex_match(text, std::regex("[0-9]{8}")))
            throw std::invalid_argument("invalid trading day in " + file_.string());
        days_.insert(text);
    }
}

void RecordedPublication::save() const
{
    create_directories_durably(file_.parent_path());
    replace_file_durably(file_, Json(days_).dump());
}

void RecordedPublication::add(const std::string& day)
{
    if (day == day_)
        merged_again_ = true;
    if (days_.insert(day).second)
        save();
}

void RecordedPublication::poll(Clock::time_point now)
{
    if (!task_.empty()) {
        const auto task = manager_.get(task_);
        if (task.state() == wire::TASK_STATE_QUEUED || task.state() == wire::TASK_STATE_RUNNING)
            return;
        if (task.state() == wire::TASK_STATE_SUCCEEDED && !merged_again_) {
            days_.erase(day_);
            save();
        } else if (task.state() != wire::TASK_STATE_SUCCEEDED) {
            log::error(
                "recorded minutes of {} did not reach research: {} {}",
                day_,
                task.error_code(),
                task.error());
            next_ = now + 1h;
        }
        task_.clear();
        day_.clear();
        merged_again_ = false;
    }
    if (days_.empty() || now < next_)
        return;
    next_ = now + 1min;
    const auto day = *days_.begin();
    wire::Request request;
    request.set_id("recorded-" + day + "-" + unique_process_id().substr(0, 8));
    request.set_provider("recorder");
    request.set_table("minutes");
    request.set_begin_day(day.substr(0, 4) + "-" + day.substr(4, 2) + "-" + day.substr(6));
    request.set_end_day(request.begin_day());
    request.set_refresh(true);
    try {
        manager_.submit(request);
        task_ = request.id();
        day_ = day;
    } catch (const Error& error) {
        // Another task of the recorder, or both workers, are busy.
        if (error.code() != ErrorCode::conflict && error.code() != ErrorCode::resource_exhausted)
            throw;
    }
}

} // namespace asterion::market_data::acquisition
