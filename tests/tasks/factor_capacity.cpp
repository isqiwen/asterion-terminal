// Offline capacity probe. Generated inputs are synthetic fixtures, never provider data.
#include "factor_engine.hpp"
#include "evaluation.hpp"
#include "data/bar_fixture.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/artifact.hpp>
#include <CLI/CLI.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sys/resource.h>

using namespace asterion;

namespace {

factor::proto::FactorInput
fixture(unsigned series, unsigned rows, bool bars, unsigned window, bool volatility_factor)
{
    factor::proto::FactorInput input;
    input.set_factor(
        volatility_factor ? protocol::proto::VOLATILITY : protocol::proto::PRICE_MOMENTUM);
    input.set_full_sample(true);
    input.add_lookbacks(window);
    input.set_horizon(5);
    for (unsigned s = 0; s < series; ++s) {
        if (bars) {
            *input.add_series()->mutable_bars() = test::dataset(test::series(rows));
            continue;
        }
        auto* table = input.add_series()->mutable_table();
        const auto contract = "SHFE/cu/" + std::to_string(2020 + s / 12) + "-"
            + (s % 12 + 1 < 10 ? "0" : "") + std::to_string(s % 12 + 1);
        table->set_instrument(contract);
        table->set_snapshot(std::string(64, 'b'));
        table->set_daily_source("fixture.daily.v1");
        table->set_calendar_source("fixture.calendar.v1");
        table->set_acquired_at_ns(1790000000000000000);
        auto* month = table->add_months();
        month->set_contract(contract);
        for (unsigned i = 0; i < rows; ++i) {
            const auto day = format_trading_date(
                std::chrono::year_month_day{
                    std::chrono::sys_days{std::chrono::year{2000} / 1 / 1} + std::chrono::days{i}
                });
            auto* row = month->add_closes();
            row->set_trading_day(day);
            row->mutable_close()->set_units(
                Decimal::parse(std::to_string(1000 + s * 10 + i % (37 + s))).raw());
            row->set_volume_contracts(100);
            row->set_open_interest_contracts(50);
        }
        table->set_begin_day(month->closes(0).trading_day());
        table->set_end_day(month->closes(static_cast<int>(rows) - 1).trading_day());
    }
    input.set_dataset_revision(protocol::factor_revision(input.series()));
    protocol::validate_factor_input(input);
    return input;
}

long peak_kib()
{
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage))
        throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
    return usage.ru_maxrss / 1024;
#else
    return usage.ru_maxrss;
#endif
}

} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"Offline synthetic factor capacity probe"};
    std::string path, phase = "engine";
    unsigned series = 20, rows = 1000, window = 20;
    bool generate = false, bars = false, volatility_factor = false;
    app.add_option("--input", path)->required();
    app.add_option("--phase", phase)
        ->check(CLI::IsMember({"read", "validate", "align", "evaluate", "engine", "result"}));
    app.add_flag("--generate", generate);
    app.add_flag("--bars", bars);
    app.add_flag("--volatility", volatility_factor);
    app.add_option("--series", series)->check(CLI::Range(1, 100));
    app.add_option("--rows", rows)->check(CLI::Range(40, 200000));
    app.add_option("--window", window)->check(CLI::Range(2, 10000));
    CLI11_PARSE(app, argc, argv);
    try {
        if (generate) {
            if (std::filesystem::exists(path) || (bars && series != 1))
                throw std::invalid_argument(
                    "generation requires a new file and one series for bars");
            write_file_durably(
                path,
                fixture(series, rows, bars, window, volatility_factor).SerializeAsString());
            return 0;
        }
        Json report{{"phase", phase}};
        const auto measure = [&](const char* name, auto&& work) {
            const auto start = std::chrono::steady_clock::now();
            work();
            report[name] =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            report[std::string(name) + "_peak_kib"] = peak_kib();
        };
        std::string bytes;
        measure("read_ms", [&] {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                throw std::runtime_error("cannot open probe input");
            bytes.assign(std::istreambuf_iterator<char>{file}, {});
            if (file.bad())
                throw std::runtime_error("cannot read probe input");
        });
        factor::proto::FactorInput input;
        measure("parse_ms", [&] {
            if (!input.ParseFromString(bytes))
                throw std::invalid_argument("invalid probe input");
        });
        report["input_bytes"] = bytes.size();
        report["input_sha256"] = sha256_bytes(bytes);
        std::string{}.swap(bytes);
        if (phase == "validate")
            measure("validate_ms", [&] {
                protocol::validate_factor_input(input);
            });
        if (phase == "align" || phase == "evaluate") {
            protocol::FactorObservations observed;
            measure("align_ms", [&] {
                observed = protocol::factor_observations(input);
            });
            report["timeline_count"] = observed.order.size();
            if (phase == "evaluate") {
                std::vector<std::span<const std::optional<Decimal>>> closes(
                    observed.closes.begin(),
                    observed.closes.end());
                const std::vector<unsigned> windows(
                    input.lookbacks().begin(),
                    input.lookbacks().end());
                measure("evaluate_ms", [&] {
                    const auto result = evaluate_factor(
                        closes,
                        closes,
                        input.factor() == protocol::proto::VOLATILITY ? volatility : momentum,
                        windows,
                        input.horizon(),
                        std::nullopt);
                    report["result_rows"] = result.samples.size() + result.cross_sections.size();
                });
            }
        }
        if (phase == "engine" || phase == "result") {
            factor::proto::FactorResult result;
            measure("engine_ms", [&] {
                result = factor::run(input);
            });
            report["result_sha256"] = sha256_bytes(result.SerializeAsString());
            report["result_bytes"] = result.ByteSizeLong();
            if (phase == "result") {
                measure("result_validation_ms", [&] {
                    protocol::validate_factor_result(input, result);
                });
                report["result_json_bytes"] =
                    protocol::decode_factor_result(input, result).dump().size();
                std::size_t members = 0;
                for (const auto& section : result.cross_sections())
                    members += static_cast<std::size_t>(section.members_size());
                report["result_members"] = members;
            }
        }
        std::cout << report.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
