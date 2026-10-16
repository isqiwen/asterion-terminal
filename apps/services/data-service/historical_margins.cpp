#include "historical_margins.hpp"
#include "margins.hpp"
#include <asterion/protocol/data.hpp>

namespace asterion::data {

v1::HistoricalMargins
historical_margins(const history_files::TableStore& store, const v1::HistoricalMarginQuery& query)
{
    protocol::validate_message(query);
    if (query.contract() != shfe::contract || query.trading_days().empty()
        || query.trading_days_size() > 3)
        throw std::invalid_argument(
            "SHFE margin supports AU2512 on 2025-10-20 through 2025-10-22 only");
    // An empty snapshot previews the current head; submission always supplies the preview's head.
    const auto snapshot = query.snapshot().empty() ? store.head() : query.snapshot();
    const auto until = store.sequence(snapshot);
    if (!until)
        throw std::invalid_argument("historical margin snapshot is unavailable");
    v1::HistoricalMargins result;
    result.set_snapshot(snapshot);
    result.set_contract(query.contract());
    result.set_source(shfe::source);
    result.set_unit("fraction");
    result.set_basis("exchange_speculation");
    result.set_unit_reference("https://www.shfe.com.cn/images/generateData_settlement_f.js");
    result.set_phase_reference("https://www.shfe.com.cn/data/config/report_config_cn.dat");
    result.set_basis_reference(
        "https://www.shfe.com.cn/publicnotice/notice/202510/t20251017_829279.html");
    std::string previous;
    for (const auto& day : query.trading_days()) {
        shfe::validate_scope("SHFE", day);
        if (day <= previous)
            throw std::invalid_argument("invalid historical margin evidence");
        previous = day;
        const auto revisions =
            store.revisions({shfe::table, 1, shfe::source}, "SHFE", day, day, *until);
        if (revisions.empty())
            throw std::invalid_argument("historical margin partition is missing");
        const auto& revision = revisions.front();
        const auto raw = store.response(revision);
        if (!raw)
            throw std::invalid_argument("historical margin source evidence is missing");
        const auto reports = shfe::reports(*raw, day);
        const auto normalized =
            table_partition(history_table(shfe::table), "SHFE", day, shfe::rows(*raw, day));
        if (normalized.content != revision.content || normalized.rows != store.rows(revision))
            throw std::invalid_argument(
                "historical margin source evidence does not match stored rows");
        for (const auto& report : reports)
            if (report.long_rate != report.short_rate)
                throw std::invalid_argument(
                    "asymmetric margin cannot use the current single-rate cost schedule");
        auto* value = result.add_days();
        value->set_trading_day(day);
        value->mutable_day_start_rate()->set_units(reports[0].long_rate.raw());
        value->mutable_settlement_rate()->set_units(reports[1].long_rate.raw());
        value->set_revision_snapshot(revision.snapshot);
        value->set_content(revision.content);
        value->set_object(revision.object);
        value->set_provider_artifact(revision.provider_artifact);
        value->set_acquired_at_ns(revision.acquired_at_ns);
        value->set_day_start_url(reports[0].url);
        value->set_settlement_url(reports[1].url);
        value->set_day_start_sha256(reports[0].sha256);
        value->set_settlement_sha256(reports[1].sha256);
        value->set_day_start_updated(reports[0].updated);
        value->set_settlement_updated(reports[1].updated);
    }
    (void)protocol::decode_historical_margins(result);
    return result;
}

} // namespace asterion::data
