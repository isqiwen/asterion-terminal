#include <chrono>
#include "data/history_fixture.hpp"
#include <cstdio>
// Seed only an isolated, stopped Data/Task pair through their persistence APIs.
#include "history_minutes.hpp"
#include "task_store.hpp"
#include "tasks/task_store_support.hpp"
#include "data_store.hpp"
#include "data/data_fixture.hpp"
#include "tushare.hpp"
#include <asterion/kernel/environment.hpp>
#include <CLI/CLI.hpp>
#include <iostream>
using namespace asterion;

namespace {

// Answers Tushare trading-calendar requests with every day open, so fixtures
// keep their own dates; every other request goes to the wrapped transport.
template <class Transport>
tushare::Post calendar(Transport transport)
{
    return [transport](const std::string& body, std::stop_token stop) -> std::string {
        const auto request = Json::parse(body);
        if (request.at("api_name") != "trade_cal")
            return transport(body, stop);
        const auto& params = request.at("params");
        const auto year = std::stoi(params.at("start_date").get<std::string>().substr(0, 4));
        Json items = Json::array();
        for (std::chrono::sys_days day = std::chrono::year(year) / 1 / 1;
            day <= std::chrono::sys_days(std::chrono::year(year) / 12 / 31);
            day += std::chrono::days(1)) {
            const std::chrono::year_month_day date(day);
            char text[9];
            std::snprintf(
                text,
                sizeof text,
                "%04d%02u%02u",
                static_cast<int>(date.year()),
                static_cast<unsigned>(date.month()),
                static_cast<unsigned>(date.day()));
            items.push_back({params.at("exchange"), text, 1});
        }
        return Json{
            {"code", 0},
            {"data", {{"fields", {"exchange", "cal_date", "is_open"}}, {"items", items}}}
        }.dump();
    };
}

} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"Create an isolated native desktop minute-data fixture"};
    std::string directory;
    // The market-wide tables are seeded under the sources the installed data
    // source offers: those are the ones the Terminal's pages read.
    test::DailyTableSources tables;
    app.add_option("--directory", directory)->required();
    app.add_option("--daily-source", tables.daily)->required();
    app.add_option("--calendar-source", tables.calendar)->required();
    app.add_option("--contracts-source", tables.contracts)->required();
    CLI11_PARSE(app, argc, argv);
    try {
        const auto root = environment_path("ASTERION_NODE_DIRECTORY");
        if (environment_variable("ASTERION_TEST_NODE_ISOLATED") != "1" || !root
            || std::filesystem::canonical(directory)
                != std::filesystem::canonical(*root / "services" / "task" / "ledger"))
            throw std::invalid_argument("minute fixture requires an isolated task ledger");
        const auto data_directory = *root / "services" / "historical-data" / "ledger";
        {
            data::Store warehouse(data_directory, "historical-data", "task");
            tasks::Store store(
                directory,
                tasks::Identity{
                    "task",
                    "historical-data"
                }); // Exclusive ownership refuses a running service.
            if (store.list().tasks_size())
                throw std::invalid_argument("minute fixture requires an empty task store");
            // A continuous afternoon session: no 10:15-10:30 pause inside the range.
            const auto begin = tushare::parse_time("2023-08-25 13:00:00");
            const HistoricalBarRange range{
                {"SHFE", "cu", "2023-10"},
                1,
                begin,
                begin + 119 * 60000000000LL,
                "tushare.ft_mins",
                "CU2310.SHF"
            };
            data::v1::MinuteDownload input;
            input.set_source("tushare.ft_mins");
            input.set_source_instrument("CU2310.SHF");
            input.set_contract_id("SHFE/cu/2023-10");
            input.set_interval_minutes(1);
            input.set_begin_ns(range.begin_ns);
            input.set_end_ns(range.end_ns);
            input.set_requests_per_minute(60);
            const std::string id = "native-minute-fixture";
            tasks::submit(store, test::authorize_download(warehouse, "task", id, input));
            task::v1::TaskAttempt attempt;
            attempt.set_token(store.commit(store.claim(id)).token());
            *attempt.mutable_task() = store.get(id);
            data::v1::DownloadAllocation allocation;
            auto* identity = allocation.mutable_identity();
            identity->set_data_instance("historical-data");
            identity->set_task_instance("task");
            identity->set_task_id(id);
            identity->set_attempt(attempt.task().attempt());
            *allocation.mutable_minutes() = input;
            attempt.set_output_directory(
                test::allocate_download(warehouse, allocation).directory());
            // The requested contract's bars, whichever it is.
            tushare::Minutes provider(
                "explicit-test-fixture",
                calendar([begin](const std::string& body, auto) {
                    const auto code =
                        Json::parse(body).at("params").at("ts_code").get<std::string>();
                    std::string response =
                        R"({"code":0,"data":{"fields":["ts_code","trade_time","open","high","low","close","vol","amount","oi"],"items":[)";
                    for (int i = 0; i < 120; ++i) {
                        if (i)
                            response += ',';
                        response += "[\"" + code + "\",\""
                            + tushare::format_time(begin + i * 60000000000LL) + "\",100.00000001,"
                            + std::to_string(102 + i % 7) + ",99," + std::to_string(101 + i % 7)
                            + ".5,20,12345678.12345678,1000]";
                    }
                    return response + "]}}";
                }));
            provider.start();
            history_files::download_minutes(provider, range, attempt.output_directory());
            task::v1::TaskFinish finish;
            finish.set_id(id);
            finish.set_token(attempt.token());
            *finish.mutable_minutes() = history_files::minute_result(attempt.output_directory());
            auto completion = store.prepare_finish(finish);
            completion.prepare_payload();
            const auto prepared =
                warehouse.prepare(warehouse.verify_download(completion.download_preparation()));
            if (!(store.commit(store.prepare_publication(std::move(completion), prepared))
                        .task()
                        .state()
                    == asterion::task::v1::PUBLISHING))
                throw std::runtime_error("fixture publication was cancelled");
            store.commit(store.confirm_publication(warehouse.publish(
                warehouse.verify_publication(store.pending_publications().front()))));

            // Another contract's download, left QUEUED: only the real Agent-dispatched
            // worker may claim and finish it after Electron starts the isolated task
            // service. Its source pages are durable already, so that worker asks the
            // provider for nothing.
            auto queued = input;
            queued.set_contract_id("SHFE/zn/2023-10");
            queued.set_source_instrument("ZN2310.SHF");
            const std::string queued_id = "native-queued-minutes";
            tasks::submit(store, test::authorize_download(warehouse, "task", queued_id, queued));
            allocation.mutable_identity()->set_task_id(queued_id);
            allocation.mutable_identity()->set_attempt(1);
            *allocation.mutable_minutes() = queued;
            history_files::download_minutes(
                provider,
                history_files::minute_range(queued),
                test::allocate_download(warehouse, allocation).directory());
            if (store.get(queued_id).state() != task::v1::QUEUED
                || store.get(queued_id).attempt() != 0)
                throw std::runtime_error("queued fixture must remain queued for Agent dispatch");
        }
        // The completed download's contract's days in the market-wide daily
        // table, which the market chart reads: one store owns the table ledger at
        // a time, so after the data store above has closed.
        std::vector<test::DailyRow> days;
        for (int i = 0; i < 120; ++i) {
            using namespace std::chrono;
            days.push_back(
                {format_trading_date(
                     year_month_day(sys_days(year(2023) / January / 1) + std::chrono::days(i))),
                    std::nullopt,
                    1000,
                    test::DailyTrade{100, 102 + i % 7, 99, 101 + i % 7, 20}});
        }
        (void)test::seed_daily_table(data_directory, "SHFE/cu/2023-10", days, tables, 5);
        std::cout
            << "Created completed minute fixture, queued minute fixture with durable source " "pages and the contract's days in the daily table\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
