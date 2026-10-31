#include "history_coverage.hpp"
#include "history_minutes.hpp"
#include <asterion/futures/historical_sessions.hpp>
#include "table_series.hpp"
#include <asterion/protocol/data.hpp>
#include <set>
#include <asterion/futures/trading_days.hpp>
#include <asterion/futures/history_identity.hpp>

namespace asterion::data {

MinuteReferenceDays minute_reference_days(
    const history_files::TableStore& tables,
    const data::proto::DailyTable& daily_reference,
    const std::string& contract_id,
    const std::string& begin,
    const std::string& end)
{
    protocol::validate_daily_table(daily_reference);
    const auto until = tables.sequence(daily_reference.snapshot());
    if (!until)
        throw std::invalid_argument("minute range reference snapshot is unavailable");
    const auto identity = HistoryIdentity::parse(contract_id);
    MinuteReferenceDays result;
    auto& calendar = result.calendar;
    const auto& cal = history_table("futures.calendar");
    const auto date_column = table_column(cal, "day"), open_column = table_column(cal, "open");
    for (int year = std::stoi(begin.substr(0, 4)); year <= std::stoi(end.substr(0, 4)); ++year) {
        const auto text = std::to_string(year);
        const auto held = tables.revisions(
            {"futures.calendar", 1, daily_reference.calendar_source()},
            identity.venue,
            text,
            text,
            *until);
        for (const auto& revision : held)
            for (const auto& row : tables.rows(revision)) {
                const auto date = format_trading_date(
                    std::chrono::year_month_day(
                        std::chrono::sys_days(
                            std::chrono::days(std::get<std::int64_t>(row[date_column])))));
                if (const auto* open = std::get_if<std::int64_t>(&row[open_column]))
                    calendar[date] = *open != 0;
                else
                    calendar[date] = std::nullopt;
            }
    }
    auto& traded = result.traded;
    const auto& daily = history_table("futures.daily");
    const auto contract = table_column(daily, "contract");
    const auto held = tables.revisions(
        {"futures.daily", 1, daily_reference.daily_source()},
        identity.venue,
        begin,
        end,
        *until);
    for (const auto& revision : held)
        for (const auto& row : tables.rows(revision)) {
            if (std::get<std::string>(row[contract]) != identity.key())
                continue;
            unsigned prices = 0;
            for (const auto* name : {"open", "high", "low"})
                if (std::get_if<std::int64_t>(&row[table_column(daily, name)]))
                    ++prices;
            if (prices == 3 && std::get_if<std::int64_t>(&row[table_column(daily, "close")])
                && std::get_if<std::int64_t>(&row[table_column(daily, "volume")]))
                traded[revision.period] = true;
            else if (prices == 0)
                traded[revision.period] = false;
            else
                traded[revision.period] = std::nullopt;
        }
    return result;
}

data::proto::MinuteRangeEvidence minute_range_evidence(
    const history_files::Archive& archive,
    const history_files::TableStore& tables,
    const data::proto::MinuteRangeQuery& query)
{
    protocol::validate_minute_range_query(query);
    const auto record = archive.get(query.dataset_id());
    if (!record.has_minute_result())
        throw std::invalid_argument("range audit requires a readable minute version");
    const auto evidence =
        history_files::minute_evidence(record.minute_result().directory(), query.dataset_id());
    const auto identity = HistoryIdentity::parse(record.minutes().contract_id());
    const auto references = minute_reference_days(
        tables,
        query.daily(),
        identity.key(),
        query.begin_day(),
        query.end_day());
    const auto& calendar = references.calendar;
    const auto& traded = references.traded;
    std::map<std::string, data::proto::MinuteDayEvidence> observed;
    for (const auto& day : evidence.days())
        observed.emplace(day.trading_day(), day);
    data::proto::MinuteRangeEvidence result;
    *result.mutable_query() = query;
    result.set_contract_id(identity.key());
    result.set_minute_source(record.minutes().source());
    result.set_interval_minutes(record.minutes().interval_minutes());
    result.set_request_begin_ns(evidence.begin_ns());
    result.set_request_end_ns(evidence.end_ns());
    result.set_unknown_rows(evidence.unknown_rows());
    const auto last = std::chrono::sys_days(parse_trading_date(query.end_day()));
    for (auto day = std::chrono::sys_days(parse_trading_date(query.begin_day())); day <= last;
        day += std::chrono::days(1)) {
        const auto date = format_trading_date(std::chrono::year_month_day(day));
        auto* row = result.add_days();
        row->set_trading_day(date);
        const auto minutes = observed.find(date);
        if (minutes != observed.end())
            *row->mutable_observed() = minutes->second;
        const auto c = calendar.find(date), d = traded.find(date);
        const auto keep = [](const historical_sessions::Source& source,
                              data::proto::SessionClosureEvidence* target) {
            target->set_rule_id(source.id);
            target->set_version(source.version);
            target->set_exchange(source.exchange);
            target->set_first_day(source.first_day);
            target->set_last_day(source.last_day);
            target->set_published_day(source.published_day);
            target->set_source_url(source.url);
        };
        const auto closed = historical_sessions::reviewed_closure(
            identity.venue,
            date,
            historical_sessions::Part::day);
        if (const auto night = historical_sessions::reviewed_closure(
                identity.venue,
                date,
                historical_sessions::Part::evening))
            keep(*night, row->mutable_evening_closure());
        if (closed) {
            keep(*closed, row->mutable_official_closure());
            row->set_state(
                minutes != observed.end() || (c != calendar.end() && c->second.value_or(false))
                        || (d != traded.end() && d->second.value_or(false))
                    ? data::proto::RANGE_RULE_CONFLICT
                    : data::proto::RANGE_OFFICIAL_CLOSED);
        } else if (c == calendar.end() || !c->second.has_value())
            row->set_state(data::proto::RANGE_UNKNOWN_CALENDAR);
        else if (!*c->second)
            row->set_state(
                minutes == observed.end() ? data::proto::RANGE_CLOSED
                                          : data::proto::RANGE_CALENDAR_CONFLICT);
        else if (d == traded.end() || !d->second.has_value())
            row->set_state(data::proto::RANGE_UNKNOWN_DAILY);
        else if (minutes != observed.end())
            row->set_state(data::proto::RANGE_OBSERVED);
        else if (!*d->second)
            row->set_state(data::proto::RANGE_NO_AUCTION);
        else
            row->set_state(
                evidence.unknown_rows() ? data::proto::RANGE_UNKNOWN_LABELS
                                        : data::proto::RANGE_UNCOVERED);
    }
    return result;
}

data::proto::MinuteEvidence minute_evidence(
    const history_files::Archive& archive,
    const data::proto::MinuteEvidenceQuery& query)
{
    protocol::validate_minute_evidence_query(query);
    const auto record = archive.get(query.dataset_id());
    auto result =
        history_files::minute_evidence(record.minute_result().directory(), query.dataset_id());
    const auto offset = std::min(query.offset(), result.total_days());
    const auto count = std::min(query.limit(), result.total_days() - offset);
    result.mutable_days()->DeleteSubrange(offset + count, result.total_days() - offset - count);
    result.mutable_days()->DeleteSubrange(0, offset);
    result.set_offset(offset);
    result.set_limit(query.limit());
    return result;
}

MinuteCoverage minute_coverage(
    const history_files::TableStore& tables,
    const data::proto::DailyTable& daily,
    const std::string& contract,
    const data::proto::MinuteEvidence& evidence)
{
    MinuteCoverage result;
    if (evidence.days().empty() || evidence.unknown_rows())
        return result;
    std::set<std::string> minute_days;
    for (const auto& day : evidence.days())
        minute_days.insert(day.trading_day());
    // This compares whole trading days; it is no proof of intraday completeness.
    auto days = traded_days(tables, daily, contract, *minute_days.begin(), *minute_days.rbegin());
    result.compared_first = std::move(days.first);
    result.compared_last = std::move(days.last);
    result.traded = static_cast<unsigned>(days.traded.size());
    for (auto& day : days.traded)
        if (!minute_days.contains(day))
            result.uncovered.push_back(std::move(day));
    return result;
}

data::proto::HistoryCoverages history_coverage(
    const history_files::Archive& archive,
    const history_files::TableStore& tables,
    const data::proto::HistoryCoverageQuery& query)
{
    protocol::validate_message(query);
    protocol::validate_daily_table(query.daily());
    const auto& filter = query.filter();
    data::proto::HistoryCoverages result;
    for (const auto& item : archive.datasets(
             {filter.venue(), filter.product(), filter.contract_id(), filter.source()})) {
        const auto record = archive.get(item.id);
        if (!record.has_minute_result())
            continue;
        if (result.items_size() >= 10000)
            throw std::invalid_argument("historical archive query exceeds limit; narrow filters");
        const auto evidence = history_files::minute_evidence(
            std::filesystem::path(
                std::u8string(
                    record.minute_result().directory().begin(),
                    record.minute_result().directory().end())),
            item.id);
        auto* row = result.add_items();
        row->set_contract_id(item.contract.key());
        row->set_minute_dataset_id(item.id);
        row->set_minute_source(item.source);
        row->set_interval_minutes(item.interval_minutes);
        row->set_minute_days(evidence.total_days());
        row->set_unknown_rows(evidence.unknown_rows());
        if (evidence.days().empty())
            continue;
        row->set_minute_first(evidence.days().begin()->trading_day());
        row->set_minute_last(evidence.days().rbegin()->trading_day());
        const auto coverage = minute_coverage(tables, query.daily(), item.contract.key(), evidence);
        row->set_compared_first(coverage.compared_first);
        row->set_compared_last(coverage.compared_last);
        row->set_traded_days(coverage.traded);
        row->set_uncovered(static_cast<unsigned>(coverage.uncovered.size()));
        for (std::size_t i = 0; i < coverage.uncovered.size() && i < 50; ++i)
            row->add_uncovered_days(coverage.uncovered[i]);
    }
    return result;
}

} // namespace asterion::data
