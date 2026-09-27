#pragma once
#include <asterion/protocol/data.hpp>
#include <filesystem>
#include <functional>
#include <stop_token>
namespace asterion::data_pipeline {
data::v1::CsvSnapshot capture_csv(const data::v1::CsvImport &input,
                                  std::stop_token stop = {});
data::v1::DatasetPublication import_snapshot(
    const data::v1::CsvSnapshot &input, std::stop_token stop = {},
    const std::function<void(std::size_t, std::size_t)> &progress = {});
void verify_result(const data::v1::CsvSnapshot &input,
                   const data::v1::DatasetPublication &result);
data::v1::DatasetPublication import_csv(const data::v1::CsvImport &input,
                                        std::stop_token stop = {});
// One immutable publication per dedicated directory. Same-input retries are
// idempotent.
bool publish(const data::v1::DatasetPublication &publication,
             const std::filesystem::path &directory);
data::v1::DatasetPublication read(const std::filesystem::path &directory);
Json summary(const data::v1::DatasetPublication &publication);
} // namespace asterion::data_pipeline
