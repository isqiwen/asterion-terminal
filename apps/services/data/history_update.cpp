#include "history_update.hpp"
#include "history_coverage.hpp"
#include "history_minutes.hpp"
#include <asterion/domain/historical_sessions.hpp>
#include <asterion/domain/history_identity.hpp>
#include <asterion/domain/trading_days.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <algorithm>
#include <chrono>

namespace asterion::data {
namespace {

std::filesystem::path directory(const std::string& text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

} // namespace

data::proto::HistoryUpdatePlan history_update_plan(
    const history_files::Archive& archive,
    const history_files::TableStore& tables,
    const data::proto::HistoryUpdateQuery& query,
    std::int64_t now_ns)
{
    (void)protocol::decode_history_update_query(query);
    if (query.mode() == data::proto::EXTEND
        && query.end_day() >= format_shanghai_time(now_ns).substr(0, 10))
        throw std::invalid_argument("incremental downloads require a completed calendar day");
    const auto record = archive.get(query.dataset_id());
    data::proto::HistoryUpdatePlan plan;
    *plan.mutable_query() = query;
    history_files::verify_minute_result(record.minutes(), record.minute_result());
    if (record.minute_result().manifest_sha256() != query.dataset_id())
        throw std::invalid_argument("dataset archive changed during resolution");
    auto input = record.minutes();
    if (query.mode() == data::proto::EXTEND) {
        // The existing download and source timestamp contract use whole seconds.
        // Re-request a fractional boundary second instead of skipping any labels.
        const auto begin = input.end_ns() % 1000000000 ? input.end_ns() / 1000000000 * 1000000000
                                                       : input.end_ns() + 1000000000;
        const auto end = parse_shanghai_time(query.end_day() + " 23:59:59");
        if (begin > end)
            throw std::invalid_argument("no later download range is requested");
        input.set_begin_ns(begin);
        input.set_end_ns(end);
    } else {
        const auto evidence = history_files::minute_evidence(
            directory(record.minute_result().directory()),
            query.dataset_id());
        if (evidence.unknown_rows())
            throw std::invalid_argument("repair requires known trading days for every minute bar");
        if (evidence.days().empty())
            throw std::invalid_argument("repair requires known minute trading days");
        const auto contract = history_files::minute_range(input).instrument.key();
        const auto venue = HistoryIdentity::parse(contract).venue;
        plan.set_session_rules_version(historical_sessions::rules_version);
        std::map<std::string, historical_sessions::Source> closed;
        const auto last =
            std::chrono::sys_days(parse_trading_date(evidence.days().rbegin()->trading_day()));
        for (auto date =
                 std::chrono::sys_days(parse_trading_date(evidence.days().begin()->trading_day()));
            date <= last;
            date += std::chrono::days(1)) {
            const auto day = format_trading_date(std::chrono::year_month_day(date));
            if (auto source = historical_sessions::reviewed_closure(
                    venue,
                    day,
                    historical_sessions::Part::day))
                closed.emplace(day, std::move(*source));
        }
        if (!closed.empty()) {
            const auto references = minute_reference_days(
                tables,
                query.daily(),
                contract,
                closed.begin()->first,
                closed.rbegin()->first);
            for (const auto& [day, source] : closed) {
                const auto known_open = [&](const auto& facts) {
                    const auto found = facts.find(day);
                    return found != facts.end() && found->second.value_or(false);
                };
                const bool observed = std::ranges::any_of(evidence.days(), [&](const auto& row) {
                    return row.trading_day() == day;
                });
                const bool calendar = known_open(references.calendar),
                           auction = known_open(references.traded);
                if (!observed && !calendar && !auction)
                    continue;
                auto* conflict = plan.add_conflicts();
                conflict->set_day(day);
                conflict->set_observed_minutes(observed);
                conflict->set_calendar_open(calendar);
                conflict->set_daily_auction(auction);
                auto* target = conflict->mutable_closure();
                target->set_rule_id(source.id);
                target->set_version(source.version);
                target->set_exchange(source.exchange);
                target->set_first_day(source.first_day);
                target->set_last_day(source.last_day);
                target->set_published_day(source.published_day);
                target->set_source_url(source.url);
            }
        }
        if (plan.conflicts().empty()) {
            for (const auto& day :
                minute_coverage(tables, query.daily(), contract, evidence).uncovered)
                plan.add_missing_days(day);
            if (plan.missing_days().empty())
                throw std::invalid_argument("no missing whole trading days in these versions");
        }
        // Use the exact original window, including all its nights and partial
        // boundaries. A provider may still return missing days; never promise fill.
    }
    input.set_requests_per_minute(query.requests_per_minute());
    (void)history_files::minute_range(input);
    *plan.mutable_minutes() = input;
    plan.set_id(sha256_bytes(plan.SerializeAsString()));
    return plan;
}

} // namespace asterion::data
