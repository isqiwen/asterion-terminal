#include "strategy_local_bars.hpp"
#include "strategy_validation.hpp"
#include <asterion/protocol/common.hpp>
#include <google/protobuf/util/message_differencer.h>
#include <fstream>
#include <optional>
#include <set>

namespace asterion::trading {
namespace fs = std::filesystem;
namespace wire = ::strategy;
namespace pb = ::trading;

std::vector<wire::Bars> recorded_inputs(const fs::path& run, const wire::Instance& configuration)
{
    if (!fs::is_regular_file(run))
        throw std::invalid_argument("no recorded strategy instance: " + run.stem().string());
    sqlite::Database previous(run, sqlite::Database::Access::read_only);
    sqlite::Database::Statement stored(previous, "SELECT value FROM configuration");
    wire::Instance recorded;
    if (!stored.step() || !recorded.ParseFromString(stored.blob(0)))
        throw std::invalid_argument("invalid strategy record: " + run.string());
    auto wanted = configuration;
    for (auto* item : {&recorded, &wanted}) {
        item->clear_id();
        item->clear_resume_from();
        item->clear_local_bars();
        item->clear_package_version();
        item->mutable_params()->clear();
        item->clear_weight_limits();
        item->clear_warmup();
        item->clear_warmup_steps();
        item->clear_execution();
    }
    if (!google::protobuf::util::MessageDifferencer::Equals(recorded, wanted))
        throw std::invalid_argument(
            "a resumed instance needs the same package, contracts and period");
    std::vector<wire::Bars> inputs;
    sqlite::Database::Statement steps(previous, "SELECT input FROM steps ORDER BY sequence");
    while (steps.step()) {
        auto& input = inputs.emplace_back();
        if (!input.ParseFromString(steps.blob(0)))
            throw std::invalid_argument("invalid strategy record: " + run.string());
    }
    return inputs;
}

std::int64_t local_bar_retention(::common::Period period)
{
    switch (period) {
        case ::common::PERIOD_MINUTES_1:
            return 20000;
        case ::common::PERIOD_MINUTES_5:
            return 5000;
        case ::common::PERIOD_MINUTES_10:
        case ::common::PERIOD_MINUTES_15:
            return 3000;
        case ::common::PERIOD_MINUTES_30:
        case ::common::PERIOD_MINUTES_60:
            return 1500;
        default:
            return 1000;
    }
}

std::map<InstrumentId, std::string> cached_expiries(const fs::path& root)
{
    std::map<InstrumentId, std::string> out;
    const auto cache = root / "catalog.pb";
    if (!fs::is_regular_file(cache) || fs::is_symlink(cache)
        || fs::file_size(cache) > 64 * 1024 * 1024)
        return out;
    std::ifstream input(cache, std::ios::binary);
    pb::ContractCatalog catalog;
    if (!catalog.ParseFromIstream(&input) || catalog.contracts_size() > 20000)
        return out;
    for (const auto& entry : catalog.contracts())
        if (!entry.expiry_day().empty())
            out.emplace(
                InstrumentId{entry.instrument().exchange(), entry.instrument().symbol()},
                entry.expiry_day());
    return out;
}

std::vector<wire::Bars> load_local_bars(
    sqlite::Database& db,
    const wire::Instance& configuration,
    bool allow_empty,
    const std::map<InstrumentId, std::string>& expiries)
{
    std::map<std::pair<std::string, std::string>, int> fixed_order;
    for (int i = 0; i < configuration.instruments_size(); ++i) {
        const auto& id = configuration.instruments(i);
        fixed_order.emplace(std::pair{id.exchange(), id.symbol()}, i);
    }

    struct LocalSeries
    {
        int slot = 0;
        std::size_t active = 0;
        std::string decided_day;
        std::optional<Decimal> pending_roll_ratio;
        std::vector<std::pair<std::string, std::string>> candidates;
        std::vector<std::string> months;
        std::vector<std::string> expiries;
    };

    std::string warmup_last_day;
    if (configuration.has_warmup() && configuration.warmup().bars_size() > 0) {
        const auto& last_batch =
            configuration.warmup().bars(configuration.warmup().bars_size() - 1);
        if (last_batch.bars_size() > 0)
            warmup_last_day = last_batch.bars(0).trading_day();
    }

    std::vector<LocalSeries> series_list;
    std::map<std::pair<std::string, std::string>, std::pair<std::size_t, std::size_t>>
        series_lookup;
    for (int s = 0; s < configuration.series_size(); ++s) {
        const auto& series = configuration.series(s);
        std::size_t active = 0;
        if (configuration.has_warmup()) {
            for (int b = configuration.warmup().bars_size() - 1; b >= 0; --b) {
                bool matched = false;
                for (const auto& bar : configuration.warmup().bars(b).bars())
                    for (int c = 0; c < series.contracts_size(); ++c)
                        if (series.contracts(c).instrument().exchange()
                                == bar.instrument().exchange()
                            && series.contracts(c).instrument().symbol()
                                == bar.instrument().symbol()) {
                            active = static_cast<std::size_t>(c);
                            matched = true;
                        }
                if (matched)
                    break;
            }
        }
        LocalSeries local;
        local.slot = configuration.instruments_size() + s;
        local.active = active;
        local.decided_day = warmup_last_day;
        for (int c = 0; c < series.contracts_size(); ++c) {
            const auto& entry = series.contracts(c);
            const std::pair key{entry.instrument().exchange(), entry.instrument().symbol()};
            local.candidates.push_back(key);
            local.months.push_back(entry.delivery_month());
            const auto exp = expiries.find(InstrumentId{key.first, key.second});
            local.expiries.push_back(exp == expiries.end() ? std::string{} : exp->second);
            if (static_cast<std::size_t>(c) >= active)
                series_lookup.emplace(
                    key,
                    std::pair{series_list.size(), static_cast<std::size_t>(c)});
        }
        series_list.push_back(std::move(local));
    }
    std::map<std::int64_t, std::map<int, wire::Bar>> grouped;
    std::map<std::int64_t, std::map<std::pair<std::size_t, std::size_t>, wire::Bar>> series_bars;
    std::vector<std::vector<std::map<std::string, Decimal>>> series_oi(series_list.size());
    std::vector<std::vector<std::map<std::string, Decimal>>> series_close(series_list.size());
    std::vector<std::vector<std::optional<Decimal>>> prev_bar_close(series_list.size());
    for (std::size_t i = 0; i < series_list.size(); ++i) {
        series_oi[i].resize(series_list[i].candidates.size());
        series_close[i].resize(series_list[i].candidates.size());
        prev_bar_close[i].resize(series_list[i].candidates.size());
    }
    if (configuration.has_warmup()) {
        for (const auto& batch : configuration.warmup().bars()) {
            for (const auto& bar : batch.bars()) {
                const std::pair key{bar.instrument().exchange(), bar.instrument().symbol()};
                if (const auto found = series_lookup.find(key); found != series_lookup.end()) {
                    if (bar.has_open_interest() && !bar.open_interest().text().empty())
                        series_oi[found->second.first][found->second.second][bar.trading_day()] =
                            Decimal::parse(bar.open_interest().text());
                    if (bar.has_close() && !bar.close().text().empty()) {
                        const auto close = Decimal::parse(bar.close().text());
                        series_close[found->second.first][found->second.second][bar.trading_day()] =
                            close;
                        prev_bar_close[found->second.first][found->second.second] = close;
                    }
                }
            }
        }
    }
    std::set<std::int64_t> timestamps;
    sqlite::Database::Statement query(
        db,
        "SELECT end_ms, exchange, symbol, bar FROM bars WHERE period=? ORDER BY end_ms ASC");
    query.bind(1, static_cast<std::int64_t>(configuration.period()));
    while (query.step()) {
        const auto end_ms = query.integer(0);
        const std::pair key{query.text(1), query.text(2)};
        const auto found = fixed_order.find(key);
        const auto sfound = series_lookup.find(key);
        if (found == fixed_order.end() && sfound == series_lookup.end())
            continue;
        wire::Bar bar;
        if (!bar.ParseFromString(query.blob(3)))
            throw std::runtime_error("invalid locally persisted strategy bar");
        timestamps.insert(end_ms);
        if (sfound != series_lookup.end()) {
            if (bar.has_open_interest() && !bar.open_interest().text().empty())
                series_oi[sfound->second.first][sfound->second.second][bar.trading_day()] =
                    Decimal::parse(bar.open_interest().text());
            if (bar.has_close() && !bar.close().text().empty())
                series_close[sfound->second.first][sfound->second.second][bar.trading_day()] =
                    Decimal::parse(bar.close().text());
            series_bars[end_ms][sfound->second] = std::move(bar);
        } else {
            grouped[end_ms][found->second] = std::move(bar);
        }
    }
    std::vector<wire::Bars> inputs;
    for (const auto end_ms : timestamps) {
        std::map<int, wire::Bar> items;
        if (const auto fixed_at = grouped.find(end_ms); fixed_at != grouped.end())
            items = std::move(fixed_at->second);
        if (const auto sat = series_bars.find(end_ms); sat != series_bars.end()) {
            for (std::size_t s = 0; s < series_list.size(); ++s) {
                auto& series = series_list[s];
                std::string day;
                for (std::size_t c = series.active; c < series.candidates.size(); ++c) {
                    if (const auto it = sat->second.find({s, c}); it != sat->second.end()) {
                        day = it->second.trading_day();
                        break;
                    }
                }
                if (day.empty())
                    continue;
                auto prev_val = [](const std::map<std::string, Decimal>& table,
                                    const std::string& target_day) -> std::optional<Decimal> {
                    auto it = table.lower_bound(target_day);
                    if (it != table.begin()) {
                        --it;
                        return it->second;
                    }
                    return std::nullopt;
                };
                if (day > series.decided_day) {
                    const bool had_history = !series.decided_day.empty();
                    std::optional<std::size_t> chosen;
                    Decimal best_oi;
                    for (std::size_t c = series.active; c < series.candidates.size(); ++c) {
                        if (!sat->second.contains({s, c})
                            || candidate_expiring(series.expiries, c, day))
                            continue;
                        auto oi = prev_val(series_oi[s][c], day);
                        if (!oi && !had_history) {
                            const auto& b = sat->second.at({s, c});
                            if (b.has_open_interest() && !b.open_interest().text().empty())
                                oi = Decimal::parse(b.open_interest().text());
                        }
                        if (!oi)
                            continue;
                        if (!chosen || *oi > best_oi) {
                            chosen = c;
                            best_oi = *oi;
                        }
                    }
                    if (chosen && *chosen > series.active) {
                        if (had_history) {
                            const auto old_px = prev_val(series_close[s][series.active], day);
                            const auto new_px = prev_val(series_close[s][*chosen], day);
                            if (old_px && new_px && *old_px > Decimal{} && *new_px > Decimal{})
                                series.pending_roll_ratio =
                                    divide(*new_px, *old_px, Rounding::half_up);
                        }
                        series.active = *chosen;
                    }
                    series.decided_day = day;
                }
                const auto active_it = sat->second.find({s, series.active});
                if (active_it == sat->second.end())
                    continue;
                wire::Bar dom = active_it->second;
                if (series.pending_roll_ratio && !dom.has_roll_ratio()) {
                    *dom.mutable_roll_ratio() = protocol::decimal(*series.pending_roll_ratio);
                    series.pending_roll_ratio.reset();
                }
                if (!dom.has_near_return() && dom.has_close() && !dom.close().text().empty()
                    && prev_bar_close[s][series.active]) {
                    if (const auto nr = simple_return(
                            Decimal::parse(dom.close().text()),
                            *prev_bar_close[s][series.active]))
                        *dom.mutable_near_return() = protocol::decimal(*nr);
                }
                if (!dom.has_roll_yield() || !dom.has_far_close() || !dom.has_far_return()) {
                    bool any_prev = false;
                    for (std::size_t c = series.active + 1; c < series.candidates.size(); ++c) {
                        if (sat->second.contains({s, c}) && prev_val(series_oi[s][c], day)) {
                            any_prev = true;
                            break;
                        }
                    }
                    std::optional<std::size_t> sub;
                    Decimal best_oi;
                    for (std::size_t c = series.active + 1; c < series.candidates.size(); ++c) {
                        const auto fb = sat->second.find({s, c});
                        if (fb == sat->second.end())
                            continue;
                        auto oi = prev_val(series_oi[s][c], day);
                        if (!oi && !any_prev && fb->second.has_open_interest()
                            && !fb->second.open_interest().text().empty())
                            oi = Decimal::parse(fb->second.open_interest().text());
                        if (!oi)
                            continue;
                        if (!sub || *oi > best_oi) {
                            sub = c;
                            best_oi = *oi;
                        }
                    }
                    if (sub) {
                        const auto& far_bar = sat->second.at({s, *sub});
                        if (dom.has_close() && !dom.close().text().empty() && far_bar.has_close()
                            && !far_bar.close().text().empty()) {
                            const auto far_px = Decimal::parse(far_bar.close().text());
                            if (!dom.has_far_close())
                                *dom.mutable_far_close() = far_bar.close();
                            if (!dom.has_roll_yield()) {
                                if (const auto ry = annualized_roll_yield(
                                        Decimal::parse(dom.close().text()),
                                        far_px,
                                        series.months[series.active],
                                        series.months[*sub])) {
                                    *dom.mutable_roll_yield() = protocol::decimal(*ry);
                                }
                            }
                            if (!dom.has_far_return() && prev_bar_close[s][*sub]) {
                                if (const auto fr = simple_return(far_px, *prev_bar_close[s][*sub]))
                                    *dom.mutable_far_return() = protocol::decimal(*fr);
                            }
                        }
                    }
                }
                items.emplace(series.slot, std::move(dom));
            }
            for (const auto& [sc, bar] : sat->second) {
                if (bar.has_close() && !bar.close().text().empty())
                    prev_bar_close[sc.first][sc.second] = Decimal::parse(bar.close().text());
            }
        }
        if (items.empty())
            continue;
        auto& batch = inputs.emplace_back();
        for (auto& [__, bar] : items)
            *batch.add_bars() = std::move(bar);
    }
    if (inputs.empty()) {
        if (allow_empty)
            return {};
        throw std::invalid_argument(
            "no locally recorded bars for the selected contracts and period");
    }
    return inputs;
}

void store_local_bars(
    sqlite::Database& db,
    std::map<std::tuple<int, std::string, std::string>, std::uint64_t>& write_counts,
    ::common::Period period,
    const wire::Bars& input)
{
    const auto keep = local_bar_retention(period);
    const auto period_value = static_cast<std::int64_t>(period);
    sqlite::Database::Transaction transaction(db);
    sqlite::Database::Statement insert(
        db,
        "INSERT OR REPLACE INTO bars(period, end_ms, exchange, symbol, bar) VALUES(?,?,?,?,?)");
    std::optional<sqlite::Database::Statement> prune;
    for (const auto& bar : input.bars()) {
        const auto& exchange = bar.instrument().exchange();
        const auto& symbol = bar.instrument().symbol();
        insert.reset();
        insert.bind(1, period_value)
            .bind(2, bar.end_ms())
            .bind(3, exchange)
            .bind(4, symbol)
            .bind_blob(5, bar.SerializeAsString())
            .step();
        if (++write_counts[{static_cast<int>(period), exchange, symbol}] % 64 == 0) {
            if (!prune)
                prune.emplace(
                    db,
                    "DELETE FROM bars WHERE period=? AND exchange=? AND symbol=? AND end_ms < (" "SELECT end_ms FROM bars WHERE period=? AND exchange=? AND symbol=? " "ORDER BY end_ms DESC LIMIT 1 OFFSET ?)");
            else
                prune->reset();
            prune->bind(1, period_value)
                .bind(2, exchange)
                .bind(3, symbol)
                .bind(4, period_value)
                .bind(5, exchange)
                .bind(6, symbol)
                .bind(7, keep - 1)
                .step();
        }
    }
    transaction.commit();
}

pb::RecordedBars query_recorded_bars(sqlite::Database& db, const pb::GetRecordedBars& query)
{
    if (query.period() == ::common::PERIOD_UNSPECIFIED || query.period() > ::common::PERIOD_DAY)
        throw std::invalid_argument("recorded bars require a valid period");
    const auto limit = query.limit() ? query.limit() : 500U;
    if (limit > 500U)
        throw std::invalid_argument("recorded bar pages contain 1 to 500 bars");
    const auto& exchange = query.instrument().exchange();
    const auto& symbol = query.instrument().symbol();
    sqlite::Database::Statement statement(
        db,
        "SELECT bar FROM bars WHERE period=? AND (? = '' OR exchange=?) " "AND (? = '' OR symbol=?) AND end_ms > ? " "ORDER BY end_ms ASC, exchange ASC, symbol ASC LIMIT ?");
    statement.bind(1, static_cast<std::int64_t>(query.period()))
        .bind(2, exchange)
        .bind(3, exchange)
        .bind(4, symbol)
        .bind(5, symbol)
        .bind(6, query.after_end_ms())
        .bind(7, static_cast<std::int64_t>(limit));
    pb::RecordedBars out;
    while (statement.step()) {
        auto* bar = out.add_bars();
        if (!bar->ParseFromString(statement.blob(0)))
            throw std::runtime_error("invalid locally persisted strategy bar");
    }
    return out;
}

} // namespace asterion::trading
