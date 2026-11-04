#include "acquisition/tushare.hpp"
#include "acquisition/manager.hpp"
#include "support/market_host.hpp"
#include "support/timing.hpp"
#include <asterion/market.pb.h>
#include <gtest/gtest.h>
#include <atomic>
#include <fstream>
#include <future>
#include <set>

namespace {

using namespace asterion;
using namespace std::chrono_literals;
namespace ac = market_data::acquisition;
namespace wh = market_data::warehouse;
namespace fs = std::filesystem;
using Json = nlohmann::json;
const std::string token = "fixture-token-private-0123456789";
const std::string contract_fields =
    "ts_code,exchange,name,fut_code,multiplier,trade_unit,per_unit,quote_unit,quote_unit_desc,d_mode_desc,list_date,delist_date,d_month,last_ddate,trade_time_desc";
const std::string daily_fields =
    "ts_code,trade_date,pre_close,pre_settle,open,high,low,close,settle,vol,amount,oi,oi_chg,delv_settle";

Json names(const std::string& fields)
{
    Json result = Json::array();
    std::istringstream stream(fields);
    for (std::string field; std::getline(stream, field, ',');)
        result.push_back(field);
    return result;
}

std::string table(const std::string& fields, Json rows)
{
    return Json{
        {"code", 0},
        {"data", {{"fields", names(fields)}, {"items", std::move(rows)}}}
    }.dump();
}

Json contract(
    const std::string& code,
    const std::string& exchange,
    const std::string& product,
    const std::string& month,
    const std::string& listed,
    const std::string& delisted)
{
    return Json::array(
        {code,
            exchange,
            " 铜\t合约 ",
            product,
            10,
            "吨",
            10,
            "元/吨",
            "1元/吨",
            "实物交割",
            listed,
            delisted,
            month,
            nullptr,
            "21:00-23:00"});
}

struct Vendor
{
    std::vector<std::string> calls, days;
    std::string close = "1.5e3", amount = "1234567890123456.123456";
    std::set<std::string> empty;
    Json minutes = Json::array();
    bool full_calendar = false;
    std::vector<std::size_t> minute_offsets;
    std::function<void(const Json&, std::stop_token)> before;

    std::string operator()(const std::string& body, std::stop_token stop)
    {
        const auto request = Json::parse(body);
        if (request.at("token") != token)
            throw std::runtime_error("wrong fixture credential");
        if (before)
            before(request, stop);
        const auto api = request.at("api_name").get<std::string>();
        const auto& p = request.at("params");
        calls.push_back(api);
        if (api == "fut_basic") {
            const auto exchange = p.at("exchange").get<std::string>();
            Json rows = Json::array();
            if (exchange == "SHFE")
                rows = {
                    contract("RB2605.SHF", "SHFE", "RB", "202605", "20250516", "20260515"),
                    contract("RB.SHF", "SHFE", "RB", "", "20000101", "20991231")
                };
            else if (exchange == "CZCE")
                rows = {
                    contract("AP003.ZCE", "CZCE", "AP", "202003", "20190301", "20200316"),
                    contract("AP1003.ZCE", "CZCE", "AP", "201003", "20090301", "20100316")
                };
            return table(contract_fields, rows);
        }
        if (api == "fut_trade_cal") {
            const auto exchange = p.at("exchange").get<std::string>();
            const auto year = p.at("start_date").get<std::string>().substr(0, 4);
            if (full_calendar) {
                Json rows = Json::array();
                auto previous = ac::day_of(year + "-01-01") - std::chrono::days(1);
                while (std::chrono::weekday(previous).iso_encoding() >= 6)
                    previous -= std::chrono::days(1);
                for (auto date = ac::day_of(year + "-01-01");
                    ac::day_text(date).substr(0, 4) == year;
                    date += std::chrono::days(1)) {
                    auto compact = ac::day_text(date), prior = ac::day_text(previous);
                    std::erase(compact, '-');
                    std::erase(prior, '-');
                    const bool open = std::chrono::weekday(date).iso_encoding() < 6;
                    rows.push_back({exchange, compact, open ? 1 : 0, prior});
                    if (open)
                        previous = date;
                }
                return table("exchange,cal_date,is_open,pretrade_date", rows);
            }
            return table(
                "exchange,cal_date,is_open,pretrade_date",
                {{exchange, year + "0105", 1, nullptr},
                    {exchange, year + "0106", 1, year + "0105"},
                    {exchange, year + "0107", 0, year + "0106"}});
        }
        if (api == "fut_daily") {
            const auto day = p.at("trade_date").get<std::string>();
            days.push_back(day);
            if (empty.contains(day))
                return table(daily_fields, Json::array());
            const auto code = p.at("exchange") == "CZCE" ? "AP1003.ZCE" : "RB2605.SHF";
            return "{\"code\":0,\"data\":{\"fields\":" + names(daily_fields).dump()
                + ",\"items\":[[\"" + code + "\",\"" + day + "\",3089.0,3090,3091,3100.5,3080,"
                + close + ",3095,100," + amount + ",2000,-1,null]]}}";
        }
        if (api == "ft_mins") {
            const auto offset = p.at("offset").get<std::size_t>();
            minute_offsets.push_back(offset);
            Json selected = Json::array(), rows = Json::array();
            for (const auto& row : minutes)
                if (row[1] >= p.at("start_date") && row[1] <= p.at("end_date"))
                    selected.push_back(row);
            for (auto i = offset; i < selected.size() && i < offset + 8000; ++i)
                rows.push_back(selected[i]);
            return table("ts_code,trade_time,open,high,low,close,vol,amount,oi", rows);
        }
        throw std::runtime_error("unexpected fixture API");
    }
};

ac::Factory factory(const std::shared_ptr<Vendor>& vendor)
{
    auto budget = std::make_shared<ac::Budget>(500);
    return [vendor, budget](std::stop_token stop) {
        return std::make_unique<ac::Tushare>(
            token,
            budget,
            stop,
            [vendor](const auto& body, auto token) {
                return (*vendor)(body, token);
            });
    };
}

ac::wire::StartAcquisition request(const std::string& id, const std::string& provider = "tushare")
{
    ac::wire::StartAcquisition result;
    result.set_id(id);
    result.set_provider(provider);
    result.set_table(::market::TABLE_DAILY);
    result.set_first_day("2026-01-01");
    result.set_last_day("2026-01-31");
    result.add_exchanges("SHFE");
    return result;
}

ac::wire::AcquisitionTask finished(ac::Manager& manager, const std::string& id)
{
    const auto deadline = std::chrono::steady_clock::now() + testing_support::bound(40s);
    for (;;) {
        auto task = manager.get(id);
        if (task.state() != ac::wire::TASK_STATE_RUNNING
            && task.state() != ac::wire::TASK_STATE_QUEUED)
            return task;
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("acquisition did not finish");
        std::this_thread::sleep_for(5ms);
    }
}

class Acquisition : public testing::Test
{
protected:
    fs::path root = fs::temp_directory_path() / ("asterion-acquisition-" + unique_process_id());

    void TearDown() override
    {
        fail_next_directory_syncs_for_testing(0);
        std::error_code error;
        fs::remove_all(root, error);
    }
};

TEST_F(Acquisition, ExactProviderNumbersAndDeliveryIdentitiesReachParquet)
{
    auto vendor = std::make_shared<Vendor>();
    auto source = factory(vendor)({});
    const auto listing = source->contracts("SHFE");
    ASSERT_EQ(listing.size(), 1);
    EXPECT_EQ(std::get<std::string>(listing[0][1]), "SHFE/rb/2026-05");
    EXPECT_EQ(std::get<std::string>(listing[0][5]), "铜 合约");
    auto rows = source->daily("SHFE", "2026-01-05");
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(std::get<std::string>(rows[0][7]), "1500");
    EXPECT_EQ(std::get<std::string>(rows[0][12]), "12345678901234561234.56");
    EXPECT_TRUE(std::holds_alternative<std::monostate>(rows[0][15]));
    wh::Store store(root, "tushare");
    auto writer = store.writer();
    writer.put("daily/SHFE/2026-01", rows, {"2026-01-05"});
    const auto version = writer.commit();
    EXPECT_EQ(
        std::get<std::string>(store.read(version, "daily/SHFE/2026-01")[0][12]),
        "12345678901234561234.560000");
    const auto czce = source->daily("CZCE", "2020-01-05");
    ASSERT_EQ(czce.size(), 1);
    EXPECT_EQ(std::get<std::string>(czce[0][2]), "CZCE/ap/2020-03");
    const auto calendar = source->calendar("SHFE", 2026);
    ASSERT_EQ(calendar.size(), 3);
    EXPECT_FALSE(std::get<bool>(calendar[2][2]));
    vendor->amount = "0e2147483647";
    EXPECT_THROW(source->daily("SHFE", "2026-01-05"), ac::SourceError);
}

TEST_F(Acquisition, UntradedPricesAreMissingAndRepliesNeverExposeCredentials)
{
    Vendor vendor;
    const auto post = [&](const std::string& body, std::stop_token stop) {
        auto reply = vendor(body, stop);
        if (Json::parse(body).at("api_name") == "fut_daily") {
            auto parsed = Json::parse(reply);
            auto& row = parsed["data"]["items"][0];
            row[4] = 0;
            row[5] = 0;
            row[6] = 0;
            row[7] = 8000;
            reply = parsed.dump();
        }
        return reply;
    };
    ac::Tushare source(token, std::make_shared<ac::Budget>(500), {}, post);
    const auto rows = source.daily("SHFE", "2026-01-05");
    for (auto c : {4, 5, 6, 7})
        EXPECT_TRUE(std::holds_alternative<std::monostate>(rows[0][c]));
    for (const auto& [reply, code] : std::vector<std::pair<std::string, std::string>>{
             {Json{{"code", 40001}, {"msg", "token " + token + " 不正确"}}.dump(),
                 "invalid_credential"},
             {Json{{"code", 2002}, {"msg", token}}.dump(), "permission"},
             {Json{{"code", 40001}, {"msg", "每分钟 " + token}}.dump(), "rate_limit"},
             {"broken " + token, "failed"}
         }) {
        ac::Tushare rejected(token, std::make_shared<ac::Budget>(500), {}, [&](const auto&, auto) {
            return reply;
        });
        try {
            rejected.calendar("SHFE", 2026);
            FAIL();
        } catch (const ac::SourceError& error) {
            EXPECT_EQ(error.code, code);
            EXPECT_EQ(std::string(error.what()).find(token), std::string::npos);
        }
    }
}

TEST_F(Acquisition, TruncationForeignRowsAndInvalidFieldsAreRejected)
{
    for (int failure = 0; failure < 5; ++failure) {
        Vendor vendor;
        ac::Tushare source(
            token,
            std::make_shared<ac::Budget>(500),
            {},
            [&](const auto& body, auto stop) {
                auto response = Json::parse(vendor(body, stop));
                if (Json::parse(body).at("api_name") == "fut_daily") {
                    auto& rows = response["data"]["items"];
                    if (failure == 0) {
                        const auto row = rows[0];
                        for (int n = 1; n < 2000; ++n)
                            rows.push_back(row);
                    }
                    if (failure == 1)
                        rows[0][1] = "20260106";
                    if (failure == 2)
                        rows[0][0] = "RB2605.DCE";
                    if (failure == 3)
                        rows[0][4] = "1.25";
                    if (failure == 4)
                        response["data"]["fields"][1] = "ts_code";
                }
                return response.dump();
            });
        EXPECT_THROW(source.daily("SHFE", "2026-01-05"), ac::SourceError);
    }
}

TEST_F(Acquisition, BudgetCancellationDoesNotWaitForTheMinuteOrSendAgain)
{
    auto budget = std::make_shared<ac::Budget>(1);
    budget->acquire({});
    std::stop_source stop;
    auto waiting = std::async(std::launch::async, [&] {
        EXPECT_THROW(budget->acquire(stop.get_token()), ac::SourceError);
    });
    stop.request_stop();
    ASSERT_EQ(waiting.wait_for(1s), std::future_status::ready);
    waiting.get();
    std::atomic<int> sent = 0;
    ac::Tushare source(token, budget, stop.get_token(), [&](const auto&, auto) {
        ++sent;
        return std::string{};
    });
    EXPECT_THROW(source.calendar("SHFE", 2026), ac::SourceError);
    EXPECT_EQ(sent, 0);
}

TEST_F(Acquisition, SkipsStoredDaysRefreshesOneDayAndLeavesEmptyDaysMissing)
{
    auto vendor = std::make_shared<Vendor>();
    vendor->empty.insert("20260106");
    ac::Manager manager(root, {{"tushare", factory(vendor)}});
    manager.submit(request("first"));
    const auto first = finished(manager, "first");
    ASSERT_EQ(first.state(), ac::wire::TASK_STATE_SUCCEEDED) << first.error().message();
    EXPECT_EQ(first.items(), 2);
    EXPECT_EQ(first.items_empty(), 1);
    wh::Store store(root, "tushare");
    const auto original = store.current();
    EXPECT_EQ(
        original.partitions.at("daily/SHFE/2026-01").days,
        std::vector<std::string>{"2026-01-05"});
    vendor->empty.clear();
    manager.submit(request("fill"));
    const auto filled = finished(manager, "fill");
    EXPECT_EQ(filled.state(), ac::wire::TASK_STATE_SUCCEEDED);
    EXPECT_EQ(filled.items(), 1);
    const auto before_refresh = store.current();
    auto refresh = request("refresh");
    refresh.set_first_day("2026-01-05");
    refresh.set_last_day("2026-01-05");
    refresh.set_refetch(true);
    vendor->close = "1616.25";
    manager.submit(refresh);
    EXPECT_EQ(finished(manager, "refresh").state(), ac::wire::TASK_STATE_SUCCEEDED);
    const auto current = store.read(store.current(), "daily/SHFE/2026-01");
    ASSERT_EQ(current.size(), 2);
    EXPECT_EQ(std::get<std::string>(current[0][7]), "1616.250000");
    EXPECT_EQ(std::get<std::string>(current[1][7]), "1500.000000");
    EXPECT_EQ(store.read(original, "daily/SHFE/2026-01").size(), 1);
    EXPECT_EQ(
        std::get<std::string>(store.read(before_refresh, "daily/SHFE/2026-01")[0][7]),
        "1500.000000");
    EXPECT_THROW(manager.submit(request("first")), Error);
    manager.stop();
    ac::Manager restarted(root, {{"tushare", factory(vendor)}});
    EXPECT_EQ(restarted.get("first").published_version(), first.published_version());
    EXPECT_THROW(restarted.submit(request("first")), Error);
    ac::wire::ListAcquisitionTasks query;
    const auto page = restarted.list(query);
    ASSERT_EQ(page.tasks_size(), 3);
    EXPECT_TRUE(page.next_id().empty());
    query.set_after_id(page.tasks(0).request().id());
    EXPECT_EQ(restarted.list(query).tasks(0).request().id(), page.tasks(1).request().id());
}

TEST_F(Acquisition, AProviderIsNotAskedForToday)
{
    // A provider's day so far would be stored as the whole day.
    auto vendor = std::make_shared<Vendor>();
    vendor->full_calendar = true;
    ac::Manager manager(root, {{"tushare", factory(vendor)}});
    const auto today = std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now() + std::chrono::hours(8));
    auto through_today = request("through-today");
    through_today.set_first_day(ac::day_text(today - std::chrono::days(7)));
    through_today.set_last_day(ac::day_text(today));
    manager.submit(through_today);
    ASSERT_EQ(finished(manager, "through-today").state(), ac::wire::TASK_STATE_SUCCEEDED);
    auto compact = ac::day_text(today);
    std::erase(compact, '-');
    EXPECT_FALSE(vendor->days.empty());
    EXPECT_EQ(std::count(vendor->days.begin(), vendor->days.end(), compact), 0);
}

TEST_F(Acquisition, CancellingAnInFlightRequestKeepsFetchedDaysAndDoesNotRetry)
{
    auto vendor = std::make_shared<Vendor>();
    std::promise<void> entered;
    vendor->before = [&](const Json& request, std::stop_token stop) {
        if (request.at("api_name") == "fut_daily"
            && request.at("params").at("trade_date") == "20260106") {
            entered.set_value();
            std::mutex mutex;
            std::unique_lock lock(mutex);
            std::condition_variable_any changed;
            changed.wait(lock, stop, [] {
                return false;
            });
            ac::check_stop(stop);
        }
    };
    ac::Manager manager(root, {{"tushare", factory(vendor)}});
    manager.submit(request("cancel"));
    ASSERT_EQ(entered.get_future().wait_for(5s), std::future_status::ready);
    EXPECT_THROW(manager.submit(request("overlap")), Error);
    manager.cancel("cancel");
    const auto task = finished(manager, "cancel");
    EXPECT_EQ(task.state(), ac::wire::TASK_STATE_CANCELLED);
    EXPECT_EQ(task.requests_sent(), 4);
    EXPECT_EQ(task.items_done(), 1);
    wh::Store store(root, "tushare");
    EXPECT_EQ(store.current().identity, task.published_version());
    EXPECT_EQ(store.current().partitions.at("daily/SHFE/2026-01").rows, 1);
    manager.stop();
    const auto calls = vendor->calls.size();
    ac::Manager restarted(root, {{"tushare", factory(vendor)}});
    EXPECT_EQ(restarted.get("cancel").state(), ac::wire::TASK_STATE_CANCELLED);
    EXPECT_EQ(vendor->calls.size(), calls);
}

TEST_F(Acquisition, SourceConcurrencyIsBoundedAndShutdownInterruptsProviderWaits)
{
    auto one = std::make_shared<Vendor>(), two = std::make_shared<Vendor>(),
         three = std::make_shared<Vendor>();
    std::atomic<int> entered = 0;
    auto block = [&](const Json&, std::stop_token stop) {
        ++entered;
        std::mutex mutex;
        std::unique_lock lock(mutex);
        std::condition_variable_any changed;
        changed.wait(lock, stop, [] {
            return false;
        });
        ac::check_stop(stop);
    };
    one->before = block;
    two->before = block;
    ac::Manager manager(
        root,
        {{"first", factory(one)}, {"second", factory(two)}, {"third", factory(three)}});
    manager.submit(request("one", "first"));
    manager.submit(request("two", "second"));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (entered < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    ASSERT_EQ(entered, 2);
    EXPECT_THROW(manager.submit(request("three", "third")), Error);
    const auto start = std::chrono::steady_clock::now();
    manager.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
    EXPECT_EQ(manager.get("one").state(), ac::wire::TASK_STATE_CANCELLED);
    EXPECT_EQ(manager.get("two").state(), ac::wire::TASK_STATE_CANCELLED);
}

TEST_F(Acquisition, FailedTaskPersistenceIsVisibleAndRecoveryDoesNotResubmit)
{
    auto vendor = std::make_shared<Vendor>();
    std::promise<void> entered;
    std::atomic<int> attempts = 0;
    vendor->before = [&](const Json&, std::stop_token) {
        ++attempts;
        fail_next_directory_syncs_for_testing(1);
        entered.set_value();
        throw ac::SourceError("unavailable", "fixture provider stopped");
    };
    {
        ac::Manager manager(root, {{"tushare", factory(vendor)}});
        manager.submit(request("disk"));
        ASSERT_EQ(entered.get_future().wait_for(5s), std::future_status::ready);
        manager.stop();
        EXPECT_THROW(manager.get("disk"), std::runtime_error);
        EXPECT_THROW(manager.list({}), std::runtime_error);
    }
    fail_next_directory_syncs_for_testing(0);
    ac::Manager restarted(root, {{"tushare", factory(vendor)}});
    const auto task = restarted.get("disk");
    // Failed directory synchronization permits the old or the complete new
    // record. Neither outcome may implicitly repeat a provider request.
    EXPECT_TRUE(
        task.state() == ac::wire::TASK_STATE_FAILED
        || task.state() == ac::wire::TASK_STATE_INTERRUPTED);
    EXPECT_EQ(attempts, 1);
}

TEST_F(Acquisition, MarketRestartMarksUnfinishedTasksAndServesIndependentReadOnlyClients)
{
    testing_support::MarketHost host(root);
    create_directories_durably(root / "data/acquisition");
    ac::wire::AcquisitionTask pending;
    *pending.mutable_request() = request("interrupted");
    pending.set_state(ac::wire::TASK_STATE_RUNNING);
    pending.set_started_ms(1);
    replace_file_durably(root / "data/acquisition/interrupted.pb", pending.SerializeAsString());
    host.start();
    const auto call = [&](testing_support::MarketChannel& channel, ::market::Request request) {
        request.set_id(1);
        channel.send(request.SerializeAsString(), 5s);
        ::market::Response result;
        EXPECT_TRUE(result.ParseFromString(channel.receive(5s)));
        return result;
    };
    auto one = host.open(), two = host.open();
    ::market::Request query;
    query.mutable_get_acquisition_task()->set_id("interrupted");
    const auto result = call(one, query);
    ASSERT_TRUE(result.has_get_acquisition_task()) << result.DebugString();
    EXPECT_EQ(result.get_acquisition_task().state(), ac::wire::TASK_STATE_INTERRUPTED);
    EXPECT_GT(result.get_acquisition_task().finished_ms(), 0);
    one = host.open();
    query.mutable_list_acquisition_tasks();
    EXPECT_EQ(
        call(two, query).list_acquisition_tasks().tasks(0).state(),
        ac::wire::TASK_STATE_INTERRUPTED);
    query.mutable_get_status();
    EXPECT_TRUE(call(one, query).has_get_status());
}

Json minute_row(const std::string& time, int close = 3100)
{
    return Json::array({"RB2605.SHF", time, close, close + 2, close - 1, close, 3, 30000, 2000});
}

TEST_F(Acquisition, MinutesKeepNightTradingDatesAndContractScopedPartitions)
{
    auto vendor = std::make_shared<Vendor>();
    vendor->minutes = {
        minute_row("2026-01-05 09:01:00"),
        minute_row("2026-01-05 21:01:00", 3101),
        minute_row("2026-01-06 09:01:00", 3102)
    };
    ac::Manager manager(root, {{"tushare", factory(vendor)}});
    auto task = request("minutes");
    task.set_table(::market::TABLE_MINUTES);
    task.set_contract("SHFE/rb/2026-05");
    task.set_first_day("2026-01-05");
    task.set_last_day("2026-01-07");
    manager.submit(task);
    const auto done = finished(manager, "minutes");
    ASSERT_EQ(done.state(), ac::wire::TASK_STATE_SUCCEEDED) << done.error().message();
    EXPECT_EQ(done.items_empty(), 1);
    EXPECT_EQ(vendor->minute_offsets, (std::vector<std::size_t>{0}));
    wh::Store store(root, "tushare");
    const auto version = store.current();
    ASSERT_EQ(version.partitions.size(), 2);
    EXPECT_FALSE(version.partitions.contains("minutes/SHFE/2026-01-06"));
    const auto rows = store.read(version, "minutes/SHFE/2026-01-06/rb/2026-05");
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(std::get<std::int64_t>(rows[0][4]), 1767618000000);
    EXPECT_FALSE(std::get<bool>(rows[0][5]));
    EXPECT_EQ(rows[0][11], wh::Cell{});
    EXPECT_EQ(std::get<std::string>(rows[0][9]), "3101.000000");
    task.set_id("repeat");
    manager.submit(task);
    EXPECT_EQ(finished(manager, "repeat").state(), ac::wire::TASK_STATE_SUCCEEDED);
    EXPECT_EQ(vendor->minute_offsets.size(), 1);
    task.set_contract("");
    task.set_id("no-contract");
    EXPECT_THROW(manager.submit(task), std::invalid_argument);
}

TEST_F(Acquisition, ADayByDayTaskPublishesVersionsInBatches)
{
    // Each version lists every partition, so one version per day grows the
    // store quadratically.
    auto vendor = std::make_shared<Vendor>();
    vendor->full_calendar = true;
    std::size_t weekdays = 0;
    for (auto day = ac::day_of("2025-05-19"); day <= ac::day_of("2026-05-15");
        day += std::chrono::days(1))
        if (std::chrono::weekday(day).iso_encoding() < 6) {
            vendor->minutes.push_back(minute_row(ac::day_text(day) + " 09:01:00"));
            ++weekdays;
        }
    ac::Manager manager(root, {{"tushare", factory(vendor)}});
    auto task = request("year");
    task.set_table(::market::TABLE_MINUTES);
    task.set_contract("SHFE/rb/2026-05");
    task.set_first_day("2025-05-19");
    task.set_last_day("2026-05-15");
    manager.submit(task);
    const auto done = finished(manager, "year");
    ASSERT_EQ(done.state(), ac::wire::TASK_STATE_SUCCEEDED) << done.error().message();
    wh::Store store(root, "tushare");
    EXPECT_EQ(store.current().partitions.size(), weekdays);
    const auto versions =
        std::distance(fs::directory_iterator(store.root() / "versions"), fs::directory_iterator{});
    EXPECT_LE(versions, 3);
}

TEST_F(Acquisition, FullMinutePagesAdvanceAndRepeatedPagesFail)
{
    auto vendor = std::make_shared<Vendor>();
    vendor->full_calendar = true;
    for (auto day = ac::day_of("2026-01-05"); day <= ac::day_of("2026-01-30");
        day += std::chrono::days(1)) {
        if (std::chrono::weekday(day).iso_encoding() >= 6)
            continue;
        for (int hour : {9, 10, 11, 13, 14, 21, 22})
            for (int minute = 0; minute < 60; ++minute) {
                if (hour == 10 && minute == 30)
                    continue;
                char clock[16];
                std::snprintf(clock, sizeof(clock), " %02d:%02d:00", hour, minute);
                vendor->minutes.push_back(minute_row(ac::day_text(day) + clock));
            }
    }
    ASSERT_GT(vendor->minutes.size(), 8000);
    std::reverse(vendor->minutes.begin(), vendor->minutes.end());
    auto source = factory(vendor)({});
    const auto rows = source->minutes("SHFE/rb/2026-05", "2026-01-05").rows;
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(vendor->minute_offsets, (std::vector<std::size_t>{0, 8000}));
    EXPECT_TRUE(std::is_sorted(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a[4] < b[4];
    }));
    source->minutes("SHFE/rb/2026-05", "2026-01-06");
    EXPECT_EQ(vendor->minute_offsets.size(), 2);
    ac::Tushare repeated(
        token,
        std::make_shared<ac::Budget>(500),
        {},
        [&](const std::string& body, auto stop) {
            auto request = Json::parse(body);
            if (request.at("api_name") == "ft_mins")
                request["params"]["offset"] = 0;
            return (*vendor)(request.dump(), stop);
        });
    EXPECT_THROW(repeated.minutes("SHFE/rb/2026-05", "2026-01-05"), ac::SourceError);
}

TEST_F(Acquisition, InvalidMinuteLabelsVolumesAndContractIdentityAreRejected)
{
    for (int failure = 0; failure < 4; ++failure) {
        auto vendor = std::make_shared<Vendor>();
        auto row = minute_row("2026-01-06 09:01:00");
        if (failure == 0)
            row[1] = "2026-01-06 10:30:00";
        if (failure == 1)
            row[6] = 1.5;
        if (failure == 2)
            row[0] = "CU2605.SHF";
        if (failure == 3)
            row[1] = "2026-01-06 09:01:30";
        vendor->minutes.push_back(row);
        EXPECT_THROW(
            factory(vendor)({})->minutes("SHFE/rb/2026-05", "2026-01-06"),
            ac::SourceError);
    }
}

} // namespace

TEST_F(Acquisition, RemoteManagementRequiresItsVerifiedCertificateAndSurvivesDisconnect)
{
    testing_support::MarketHost host(root);
    std::ifstream input(root / "config/market.json");
    auto config = Json::parse(input);
    config["acquisition"]["tushare"] = {{"requests_per_minute", 1U}};
    replace_file_durably(root / "config/market.json", config.dump());
    replace_file_durably(
        root / "credentials/asterion.market.tushare.token",
        "private-provider-token");
    host.start();
    const auto call = [&](const std::string& name, ::market::Request query) {
        query.set_id(1);
        auto channel = host.open(name);
        channel.send(query.SerializeAsString(), 5s);
        ::market::Response response;
        EXPECT_TRUE(response.ParseFromString(channel.receive(5s)));
        EXPECT_EQ(response.SerializeAsString().find("private-provider-token"), std::string::npos);
        return response;
    };
    ::market::Request query;
    query.mutable_list_sources();
    const auto catalog = call("reader", query);
    ASSERT_TRUE(catalog.has_list_sources()) << catalog.DebugString();
    EXPECT_FALSE(catalog.list_sources().can_manage());
    ASSERT_EQ(catalog.list_sources().sources_size(), 2);
    EXPECT_EQ(catalog.list_sources().sources(0).provider(), "recorder");
    EXPECT_EQ(catalog.list_sources().sources(1).provider(), "tushare");
    EXPECT_EQ(catalog.list_sources().sources(1).tables_size(), 4);
    EXPECT_TRUE(call("manager", query).list_sources().can_manage());
    auto* start = query.mutable_start_acquisition();
    *start = request("remote");
    // Future dates exercise real admission and persistence without provider traffic.
    start->set_first_day("9999-01-01");
    start->set_last_day("9999-01-01");
    EXPECT_EQ(call("reader", query).error().code(), ::common::ERROR_CODE_PERMISSION_DENIED);
    EXPECT_FALSE(fs::exists(root / "data/acquisition/remote.pb"));
    const auto admitted = call("manager", query);
    ASSERT_TRUE(admitted.has_start_acquisition()) << admitted.DebugString();
    EXPECT_EQ(call("manager", query).error().code(), ::common::ERROR_CODE_CONFLICT);
    query.mutable_get_acquisition_task()->set_id("remote");
    ::market::Response observed;
    for (int i = 0; i < 100; ++i) {
        observed = call("reader", query);
        if (observed.get_acquisition_task().state() == ac::wire::TASK_STATE_SUCCEEDED)
            break;
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(observed.get_acquisition_task().state(), ac::wire::TASK_STATE_SUCCEEDED);
    EXPECT_EQ(observed.get_acquisition_task().requests_sent(), 0U);
    query.mutable_cancel_acquisition()->set_id("remote");
    EXPECT_EQ(call("reader", query).error().code(), ::common::ERROR_CODE_PERMISSION_DENIED);
    EXPECT_TRUE(call("manager", query).has_cancel_acquisition());
    // An unregistered identity and a wrong server pin fail before business admission.
    ipc::create_identity(root / "stranger.crt", root / "stranger.key", "unregistered test");
    query.mutable_get_status();
    EXPECT_THROW(call("stranger", query), std::exception);
    auto tls = host.tls();
    tls.peers = {std::string(64, 'a')};
    testing_support::MarketChannel wrong_pin(host.port, tls);
    query.set_id(1);
    wrong_pin.send(query.SerializeAsString(), 2s);
    EXPECT_THROW(wrong_pin.receive(2s), std::exception);
    EXPECT_TRUE(call("reader", query).has_get_status());
}

TEST_F(Acquisition, RemotePublicTaskPublishesAfterItsAdministratorDisconnects)
{
    const auto* venue = std::getenv("ASTERION_EXCHANGE_ACCEPTANCE_SOURCE");
    const auto* day = std::getenv("ASTERION_EXCHANGE_ACCEPTANCE_DAY");
    if (!venue || !day)
        GTEST_SKIP() << "Opt-in public exchange acceptance requires a source and day";
    testing_support::MarketHost host(root);
    std::ifstream input(root / "config/market.json");
    auto config = Json::parse(input);
    config["acquisition"]["exchanges"] = {venue};
    replace_file_durably(root / "config/market.json", config.dump());
    host.start();
    const auto call = [&](const std::string& client, ::market::Request request) {
        request.set_id(1);
        auto connection = host.open(client);
        connection.send(request.SerializeAsString(), 5s);
        ::market::Response response;
        if (!response.ParseFromString(connection.receive(5s)))
            throw std::runtime_error("invalid Market reply");
        return response;
    };
    std::string provider = venue;
    std::transform(provider.begin(), provider.end(), provider.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    ::market::Request query;
    auto* start = query.mutable_start_acquisition();
    start->set_id("public");
    start->set_provider(provider);
    start->set_table(::market::TABLE_DAILY);
    start->set_first_day(day);
    start->set_last_day(day);
    start->add_exchanges(venue);
    const auto accepted = call("manager", query); // This connection now closes.
    ASSERT_TRUE(accepted.has_start_acquisition()) << accepted.DebugString();
    query.mutable_get_acquisition_task()->set_id("public");
    ac::wire::AcquisitionTask task;
    const auto deadline = std::chrono::steady_clock::now() + 75s;
    do {
        const auto reply = call("reader", query);
        ASSERT_TRUE(reply.has_get_acquisition_task()) << reply.DebugString();
        task = reply.get_acquisition_task();
        if (task.state() != ac::wire::TASK_STATE_QUEUED
            && task.state() != ac::wire::TASK_STATE_RUNNING)
            break;
        std::this_thread::sleep_for(50ms);
    } while (std::chrono::steady_clock::now() < deadline);
    ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    ASSERT_FALSE(task.published_version().empty());
    query.mutable_get_dataset_summary()->set_provider(provider);
    query.mutable_get_dataset_summary()->set_version(task.published_version());
    const auto summary = call("reader", query);
    ASSERT_TRUE(summary.has_get_dataset_summary()) << summary.DebugString();
    EXPECT_GT(summary.get_dataset_summary().tables_size(), 0);
    query.mutable_get_status();
    EXPECT_TRUE(call("reader", query).has_get_status());
    std::cout << venue << " " << day << ": TLS task published " << task.published_version()
              << " after administrator disconnect\n";
}
