#pragma once
#include <asterion/protocol/data.hpp>
#include <filesystem>
#include <stop_token>
namespace asterion::data_pipeline {
data::v1::CalendarCsvSnapshot capture_calendar_csv(const data::v1::CsvImport &,
                                                   std::stop_token stop = {});
data::v1::CalendarPublication
import_calendar_snapshot(const data::v1::CalendarCsvSnapshot &,
                         std::stop_token stop = {});
void verify_calendar_result(const data::v1::CalendarCsvSnapshot &,
                            const data::v1::CalendarPublication &);
bool publish_calendar(const data::v1::CalendarPublication &,
                      const std::filesystem::path &);
data::v1::CalendarPublication read_calendar(const std::filesystem::path &);
Json calendar_summary(const data::v1::CalendarPublication &);
} // namespace asterion::data_pipeline
