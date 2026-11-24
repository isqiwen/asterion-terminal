#include "strategy_host.hpp"
#include "alert_webhook.hpp"
#include "strategy_bars.hpp"
#include "live_session.hpp"
#include <asterion/database/sqlite/sqlite_database.hpp>
#include <asterion/futures/futures.hpp>
#include <asterion/futures/history_identity.hpp>
#include <asterion/protocol/common.hpp>
#include <asterion/base/error.hpp>
#include <asterion/base/id.hpp>
#include <asterion/base/serialization.hpp>
#include <algorithm>
#include <atomic>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/file_lock.hpp>
#include <asterion/system/ipc/local_channel.hpp>
#include <asterion/system/log.hpp>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/message_differencer.h>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <regex>
#include <map>
#include <set>
#include <tuple>
#include <unistd.h>

namespace asterion::trading {
namespace fs = std::filesystem;
namespace wire = ::strategy;
namespace pb = ::trading;
using namespace std::chrono_literals;

namespace {

std::string read(const fs::path& path, std::size_t limit)
{
    if (!fs::is_regular_file(path) || fs::is_symlink(path) || fs::file_size(path) > limit)
        throw std::invalid_argument("missing or invalid strategy file: " + path.string());
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

std::string json(const google::protobuf::Message& message)
{
    std::string result;
    if (!google::protobuf::util::MessageToJsonString(message, &result).ok())
        throw std::runtime_error("cannot encode strategy description");
    return result;
}

void package_identity(const std::string& name, const std::string& version)
{
    const std::regex component("[A-Za-z0-9][A-Za-z0-9._-]{0,63}");
    if (!std::regex_match(name, component) || !std::regex_match(version, component))
        throw std::invalid_argument(
            "package name and version must be 1-64 letters, digits, dots, underscores or hyphens, beginning with a letter or digit");
}

void validate(const wire::Package& package)
{
    const auto& m = package.manifest();
    package_identity(m.name(), m.version());
    if (m.runtime() != "python3.12-numpy2.3.5-scipy1.17.1")
        throw std::invalid_argument("unsupported strategy runtime: " + m.runtime());
    const std::regex entry(
        "[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)*:[A-Za-z_][A-Za-z0-9_]*");
    if (!std::regex_match(m.entry(), entry))
        throw std::invalid_argument("strategy entry must be module:Class");
    if (m.files_size() < 1 || m.files_size() > 64 || m.files_size() != package.files_size()
        || package.ByteSizeLong() > 600 * 1024)
        throw std::invalid_argument(
            "a strategy package has 1-64 Python files, at most 512 KiB of code");
    const std::regex path("([A-Za-z_][A-Za-z0-9_]*/)*[A-Za-z_][A-Za-z0-9_]*\\.py");
    std::set<std::string> names;
    std::size_t bytes = 0;
    for (int i = 0; i < m.files_size(); ++i) {
        const auto& name = m.files(i);
        if (!std::regex_match(name, path) || name.starts_with("asterion")
            || !names.insert(name).second || package.files(i).path() != name)
            throw std::invalid_argument("invalid or duplicate strategy source path: " + name);
        bytes += package.files(i).content().size();
    }
    if (bytes > 512 * 1024)
        throw std::invalid_argument("strategy source exceeds 512 KiB");
    auto module = m.entry().substr(0, m.entry().find(':'));
    std::replace(module.begin(), module.end(), '.', '/');
    if (!names.contains(module + ".py"))
        throw std::invalid_argument("strategy entry is not included in package files");
}

int delivery_months(const std::string& month)
{
    const auto dash = month.find('-');
    return std::stoi(month.substr(0, dash)) * 12 + std::stoi(month.substr(dash + 1));
}

// Annualized log roll yield 12 * ln(near / far) / delta_months, matching dominant.py.
std::optional<Decimal> annualized_roll_yield(
    Decimal near_price,
    Decimal far_price,
    const std::string& near_month,
    const std::string& far_month)
{
    const int gap = delivery_months(far_month) - delivery_months(near_month);
    if (gap <= 0 || near_price <= Decimal{} || far_price <= Decimal{})
        return std::nullopt;
    const double value = 12.0
        * std::log(static_cast<double>(near_price.raw()) / static_cast<double>(far_price.raw()))
        / static_cast<double>(gap);
    if (!std::isfinite(value))
        return std::nullopt;
    const double scaled = value * 1e8;
    if (scaled < static_cast<double>(std::numeric_limits<std::int64_t>::min())
        || scaled > static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    return Decimal::from_raw(std::llround(scaled));
}

std::string warmup_digest(const wire::Warmup& warmup)
{
    std::string canonical;
    for (const auto& batch : warmup.bars()) {
        canonical += "batch\n";
        for (const auto& bar : batch.bars()) {
            canonical += bar.trading_day() + "|" + std::to_string(bar.end_ms()) + "|"
                + bar.instrument().exchange() + "/" + bar.instrument().symbol() + "|"
                + bar.open().text() + "|" + bar.high().text() + "|" + bar.low().text() + "|"
                + bar.close().text() + "|" + std::to_string(bar.volume()) + "|"
                + bar.open_interest().text()
                + (bar.has_roll_ratio() ? "|" + bar.roll_ratio().text() : "")
                + (bar.has_roll_yield() ? "|ry=" + bar.roll_yield().text() : "") + "\n";
        }
    }
    return sha256_bytes(canonical);
}

std::string series_symbol(const wire::DominantSeries& series)
{
    std::string lower = series.product();
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return series.exchange() + "/" + lower;
}

int instance_slot(const wire::Instance& instance, const ::common::Instrument& id)
{
    for (int i = 0; i < instance.instruments_size(); ++i)
        if (instance.instruments(i).exchange() == id.exchange()
            && instance.instruments(i).symbol() == id.symbol())
            return i;
    for (int s = 0; s < instance.series_size(); ++s)
        for (const auto& candidate : instance.series(s).contracts())
            if (candidate.instrument().exchange() == id.exchange()
                && candidate.instrument().symbol() == id.symbol())
                return instance.instruments_size() + s;
    return -1;
}

void validate(const wire::Instance& instance)
{
    package_identity(instance.id(), "instance");
    package_identity(instance.package_name(), instance.package_version());
    const auto total_symbols = instance.instruments_size() + instance.series_size();
    if (total_symbols < 1 || static_cast<std::size_t>(total_symbols) > max_portfolio_contracts)
        throw std::invalid_argument("a strategy instance selects 1-64 native contracts");
    std::set<std::string> contracts, series_keys;
    const std::regex exchange("SHFE|INE|DCE|CZCE|CFFEX|GFEX"), symbol("[A-Za-z]+[0-9]{3,4}");
    for (const auto& value : instance.instruments()) {
        const auto name = value.exchange() + "/" + value.symbol();
        if (!std::regex_match(value.exchange(), exchange)
            || !std::regex_match(value.symbol(), symbol) || !contracts.insert(name).second)
            throw std::invalid_argument("invalid or duplicate native contract: " + name);
    }
    for (const auto& series : instance.series()) {
        if (!std::regex_match(series.exchange(), exchange) || series.product().empty()
            || series.contracts_size() < 2)
            throw std::invalid_argument(
                "dominant series requires an exchange, product and at least 2 contracts");
        const auto key = series_symbol(series);
        if (!series_keys.insert(key).second)
            throw std::invalid_argument("duplicate dominant series: " + key);
        std::string previous_month;
        for (const auto& entry : series.contracts()) {
            const auto& id = entry.instrument();
            const auto name = id.exchange() + "/" + id.symbol();
            if (id.exchange() != series.exchange() || !contracts.insert(name).second)
                throw std::invalid_argument("invalid or duplicate series contract: " + name);
            FuturesContract{
                {{id.exchange(), id.symbol()},
                    "CNY",
                    Decimal::parse("1"),
                    Decimal::parse("1"),
                    Decimal::parse("1")},
                series.product(),
                entry.delivery_month()
            }
                .validate();
            if (!previous_month.empty() && entry.delivery_month() <= previous_month)
                throw std::invalid_argument(
                    "dominant series contracts must be in ascending delivery-month order");
            previous_month = entry.delivery_month();
        }
    }
    if (instance.period() == ::common::PERIOD_UNSPECIFIED
        || instance.period() > ::common::PERIOD_DAY)
        throw std::invalid_argument("strategy bars are daily or of 1 to 60 minutes");
    if (instance.has_weight_limits()
        && (!instance.weight_limits().has_gross() || !instance.weight_limits().has_net()))
        throw std::invalid_argument(
            "strategy weight budget requires explicit gross and net limits");
    auto stripped = instance;
    if (stripped.has_warmup())
        stripped.mutable_warmup()->clear_bars();
    if (stripped.ByteSizeLong() > 65536 || instance.ByteSizeLong() > 2 * 1024 * 1024)
        throw std::invalid_argument("strategy instance configuration exceeds 64 KiB");
    if (!instance.resume_from().empty()) {
        package_identity(instance.resume_from(), "instance");
        if (instance.resume_from() == instance.id())
            throw std::invalid_argument("a strategy instance cannot resume from itself");
        if (instance.local_bars())
            throw std::invalid_argument(
                "a strategy instance resumes from a record or from local bars, not both");
    }
    if (instance.has_warmup()) {
        const auto& warmup = instance.warmup();
        if (warmup.dataset_version().empty() || warmup.bars_size() == 0)
            throw std::invalid_argument("strategy warmup requires dataset_version and bars");
        package_identity(warmup.dataset_version(), "version");
        std::int64_t previous_end_ms = 0;
        for (const auto& batch : warmup.bars()) {
            if (batch.bars_size() == 0)
                throw std::invalid_argument("strategy warmup contains an empty bar batch");
            const auto end_ms = batch.bars(0).end_ms();
            if (end_ms <= previous_end_ms)
                throw std::invalid_argument(
                    "strategy warmup bars must be in strictly ascending time order");
            previous_end_ms = end_ms;
            int previous_index = -1;
            for (const auto& bar : batch.bars()) {
                if (bar.end_ms() != end_ms || bar.trading_day().empty())
                    throw std::invalid_argument(
                        "strategy warmup bar batch has inconsistent timestamps");
                if (bar.has_roll_ratio() && Decimal::parse(bar.roll_ratio().text()) <= Decimal{})
                    throw std::invalid_argument("strategy warmup roll_ratio must be positive");
                if (bar.has_roll_yield())
                    (void)Decimal::parse(bar.roll_yield().text());
                const int index = instance_slot(instance, bar.instrument());
                if (index <= previous_index)
                    throw std::invalid_argument(
                        "strategy warmup bars must match instance instruments in order");
                previous_index = index;
            }
        }
        if (warmup.sha256() != warmup_digest(warmup))
            throw std::invalid_argument("strategy warmup sha256 digest does not match bars");
    }
}

// The bars an ended instance received, in order. Its package, contracts and
// period must match; parameters, package version and weight limits may be updated.
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

bool candidate_expiring(
    const std::vector<std::string>& expiries,
    std::size_t index,
    const std::string& day)
{
    if (index >= expiries.size() || expiries[index].empty() || day < expiries[index])
        return false;
    for (std::size_t later = index + 1; later < expiries.size(); ++later)
        if (expiries[later].empty() || expiries[later] > day)
            return true;
    return false;
}

std::vector<wire::Bars> load_local_bars(
    sqlite::Database& db,
    const wire::Instance& configuration,
    bool allow_empty,
    const std::map<InstrumentId, std::string>& expiries = {})
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
    for (std::size_t i = 0; i < series_list.size(); ++i) {
        series_oi[i].resize(series_list[i].candidates.size());
        series_close[i].resize(series_list[i].candidates.size());
    }
    if (configuration.has_warmup()) {
        for (const auto& batch : configuration.warmup().bars()) {
            for (const auto& bar : batch.bars()) {
                const std::pair key{bar.instrument().exchange(), bar.instrument().symbol()};
                if (const auto found = series_lookup.find(key); found != series_lookup.end()) {
                    if (bar.has_open_interest() && !bar.open_interest().text().empty())
                        series_oi[found->second.first][found->second.second][bar.trading_day()] =
                            Decimal::parse(bar.open_interest().text());
                    if (bar.has_close() && !bar.close().text().empty())
                        series_close[found->second.first][found->second.second][bar.trading_day()] =
                            Decimal::parse(bar.close().text());
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
                if (!dom.has_roll_yield()) {
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
                            if (const auto ry = annualized_roll_yield(
                                    Decimal::parse(dom.close().text()),
                                    Decimal::parse(far_bar.close().text()),
                                    series.months[series.active],
                                    series.months[*sub])) {
                                *dom.mutable_roll_yield() = protocol::decimal(*ry);
                            }
                        }
                    }
                }
                items.emplace(series.slot, std::move(dom));
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

} // namespace

struct StrategyHost::State
{
    struct LiveSeries
    {
        struct DayStats
        {
            std::optional<Decimal> open_interest;
            std::optional<Decimal> prev_open_interest;
            std::optional<Decimal> prev_settlement;
        };

        std::string symbol;
        std::vector<InstrumentId> candidates;
        std::vector<std::string> delivery_months;
        std::vector<std::string> expiry_days;
        std::size_t active_index = 0;
        std::string decided_day;
        std::optional<Decimal> pending_roll_ratio;
        std::vector<std::map<std::string, DayStats>> history;
    };

    State(
        fs::path installed,
        fs::path account,
        ctp::Configuration configuration,
        std::shared_ptr<AlertWebhook> webhook,
        std::shared_ptr<const Clock> injected_clock)
        : installation(std::move(installed)),
          root(std::move(account) / "strategies"),
          market(std::move(configuration)),
          alerts(std::move(webhook)),
          clock(injected_clock ? std::move(injected_clock) : default_clock())
    {
        create_directories_durably(root / "packages");
        lock = std::make_unique<FileLock>(root, "owner");
        local_bars = std::make_unique<sqlite::Database>(root / "bars.sqlite");
        local_bars->execute(
            "CREATE TABLE IF NOT EXISTS bars (" "period INTEGER NOT NULL, " "end_ms INTEGER NOT NULL, " "exchange TEXT NOT NULL, " "symbol TEXT NOT NULL, " "bar BLOB NOT NULL, " "PRIMARY KEY(period, end_ms, exchange, symbol))");
        local_bars->execute(
            "CREATE INDEX IF NOT EXISTS bars_contract_end ON bars(period, exchange, symbol, end_ms DESC)");
        if (fs::exists(root / "instance.pb")) {
            if (!instance.ParseFromString(read(root / "instance.pb", 131072)))
                throw std::invalid_argument(
                    "invalid strategy instance: " + (root / "instance.pb").string());
            if (instance.state() == pb::INSTANCE_STATE_WARMING
                || instance.state() == pb::INSTANCE_STATE_READY
                || instance.state() == pb::INSTANCE_STATE_STARTING) {
                instance.set_state(pb::INSTANCE_STATE_INTERRUPTED);
                instance.set_error("Trading restarted; explicitly start a new strategy instance");
                instance.set_process_id(0);
                save();
                if (alerts && instance.has_instance())
                    alerts->notify(
                        "strategy_failed",
                        "strategy_interrupted:" + instance.instance().id(),
                        instance.instance().id() + ": " + instance.error(),
                        {{"instance_id", instance.instance().id()},
                            {"state", "interrupted"},
                            {"error", instance.error()}});
            }
        }
    }

    ~State()
    {
        try {
            close();
        } catch (const std::exception& error) {
            log::error("strategy shutdown failed: {}", error.what());
        }
    }

    void save()
    {
        replace_file_durably(root / "instance.pb", instance.SerializeAsString());
        if (alerts && instance.state() == pb::INSTANCE_STATE_FAILED && instance.has_instance())
            alerts->notify(
                "strategy_failed",
                "strategy_failed:" + instance.instance().id(),
                instance.instance().id() + ": " + instance.error(),
                {{"instance_id", instance.instance().id()},
                    {"state", "failed"},
                    {"error", instance.error()}});
    }

    void close()
    {
        std::exception_ptr failure;
        if (authority) {
            try {
                account->release_strategy(authority).get();
                authority.reset();
            } catch (...) {
                failure = std::current_exception();
            }
        }
        if (!authority)
            account = nullptr;
        execution = {};
        quotes.clear();
        live_series.clear();
        live_sequence = 0;
        subscribed_contracts.clear();
        next_subscription_retry = {};
        next_feed_reconnect = {};
        next_target_heartbeat = {};
        last_awaited_day.clear();
        running.store(false, std::memory_order_release);
        channel.close();
        if (child) {
            child->request_stop();
            child.reset();
        }
        feed.reset();
        bars.reset();
        records.reset();
        if (!endpoint.empty()) {
            std::error_code ignored;
            fs::remove_all(endpoint.parent_path(), ignored);
            endpoint.clear();
        }
        if (failure)
            std::rethrow_exception(failure);
    }

    void record_event(pb::InstanceEventKind kind, const std::string& event)
    {
        const auto now = utc_now_ms(*clock);
        sqlite::Database::Statement insert(*records, "INSERT INTO events VALUES(?,?,?)");
        insert.bind(1, now).bind(2, static_cast<std::int64_t>(kind)).bind(3, event).step();
        log::info("strategy {}: {}", instance.instance().id(), event);
        auto* added = instance.add_events();
        added->set_time_ms(now);
        added->set_text(event);
        added->set_kind(kind);
        if (instance.events_size() > 20)
            instance.mutable_events()->DeleteSubrange(0, 1);
        save();
        if (alerts && kind == pb::INSTANCE_EVENT_KIND_BATCH_ABANDONED)
            alerts->notify(
                "batch_abandoned",
                "batch_abandoned:" + instance.instance().id() + ":"
                    + std::to_string(++abandoned_batches),
                instance.instance().id() + ": " + event,
                {{"instance_id", instance.instance().id()}, {"reason", event}});
    }

    void require_running(const std::string& id) const
    {
        if (!authority || instance.instance().id() != id)
            throw Error(ErrorCode::conflict, "strategy instance changed; inspect before acting");
    }

    void pause(const pb::PauseStrategy& request)
    {
        require_running(request.id());
        account->pause_strategy(authority, request.paused()).get();
        instance.set_paused(request.paused());
        if (request.paused())
            record_event(pb::INSTANCE_EVENT_KIND_PAUSED, "paused");
        else
            record_event(pb::INSTANCE_EVENT_KIND_RESUMED, "resumed");
    }

    void flatten(const pb::InstanceId& request)
    {
        require_running(request.id());
        account->flatten_strategy(authority, latest_quotes()).get();
        instance.set_paused(true);
        record_event(pb::INSTANCE_EVENT_KIND_FLATTEN_REQUESTED, "flatten requested");
    }

    std::vector<MarketQuote> latest_quotes() const
    {
        std::vector<MarketQuote> result;
        for (const auto& [_, quote] : quotes)
            result.push_back(quote);
        return result;
    }

    void store_local_bars(::common::Period period, const wire::Bars& input)
    {
        const auto keep = local_bar_retention(period);
        const auto period_value = static_cast<std::int64_t>(period);
        sqlite::Database::Transaction transaction(*local_bars);
        sqlite::Database::Statement insert(
            *local_bars,
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
            if (++bar_write_counts[{static_cast<int>(period), exchange, symbol}] % 64 == 0) {
                if (!prune)
                    prune.emplace(
                        *local_bars,
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

    std::set<std::string> awaited_contracts(const std::string& day = {}) const
    {
        std::set<std::string> out;
        for (const auto& id : instance.instance().instruments())
            out.insert(id.exchange() + "/" + id.symbol());
        for (const auto& series : live_series) {
            const auto& active = series.candidates[series.active_index];
            out.insert(active.venue + "/" + active.symbol);
            if (!day.empty()) {
                if (const auto sub = choose_sub_dominant(series, day))
                    out.insert(
                        series.candidates[*sub].venue + "/" + series.candidates[*sub].symbol);
            }
            if (!day.empty() && day > series.decided_day) {
                const bool expiring =
                    candidate_expiring(series.expiry_days, series.active_index, day);
                const auto active_oi = prev_open_interest(series, series.active_index, day);
                for (std::size_t m = series.active_index + 1; m < series.candidates.size(); ++m) {
                    const auto oi = prev_open_interest(series, m, day);
                    if (expiring || (oi && (!active_oi || *oi > *active_oi)))
                        out.insert(series.candidates[m].venue + "/" + series.candidates[m].symbol);
                }
            }
        }
        return out;
    }

    void observe_series_quote(const MarketQuote& observed)
    {
        if (observed.trading_day.size() != 8)
            return;
        const auto day = observed.trading_day.substr(0, 4) + "-" + observed.trading_day.substr(4, 2)
            + "-" + observed.trading_day.substr(6, 2);
        bool matched = false;
        bool awaited_changed = day != last_awaited_day;
        for (auto& series : live_series) {
            const auto found = std::ranges::find(series.candidates, observed.instrument);
            if (found == series.candidates.end())
                continue;
            matched = true;
            auto [it, inserted] =
                series.history[static_cast<std::size_t>(found - series.candidates.begin())]
                    .try_emplace(day);
            auto& stats = it->second;
            if (inserted)
                awaited_changed = true;
            if (observed.previous_settlement && *observed.previous_settlement > Decimal{})
                stats.prev_settlement = observed.previous_settlement;
            if (observed.open_interest && observed.open_interest_change) {
                const auto prev_oi = *observed.open_interest - *observed.open_interest_change;
                if (stats.prev_open_interest != prev_oi) {
                    stats.prev_open_interest = prev_oi;
                    awaited_changed = true;
                }
            }
            if (observed.open_interest) {
                if (!stats.prev_open_interest && stats.open_interest != observed.open_interest)
                    awaited_changed = true;
                stats.open_interest = observed.open_interest;
            }
        }
        if (matched && bars && awaited_changed) {
            last_awaited_day = day;
            bars->set_awaited(awaited_contracts(day));
        }
    }

    static std::optional<Decimal>
    prev_open_interest(const LiveSeries& series, std::size_t index, const std::string& day)
    {
        const auto& map = series.history[index];
        if (const auto today = map.find(day);
            today != map.end() && today->second.prev_open_interest)
            return today->second.prev_open_interest;
        auto before = map.lower_bound(day);
        if (before != map.begin()) {
            --before;
            return before->second.open_interest;
        }
        return std::nullopt;
    }

    static Decimal roll_ratio(const LiveSeries& series, std::size_t next, const std::string& day)
    {
        const auto settlement = [&](std::size_t index) {
            const auto it = series.history[index].find(day);
            if (it == series.history[index].end() || !it->second.prev_settlement)
                throw std::runtime_error("dominant roll lacks previous settlement price on " + day);
            return *it->second.prev_settlement;
        };
        return divide(settlement(next), settlement(series.active_index), Rounding::half_up);
    }

    static std::optional<std::size_t> choose_dominant(
        const LiveSeries& series,
        const std::string& day,
        bool had_history,
        const wire::Bars* raw = nullptr)
    {
        std::optional<std::size_t> chosen;
        Decimal best_oi;
        for (std::size_t m = series.active_index; m < series.candidates.size(); ++m) {
            if (candidate_expiring(series.expiry_days, m, day))
                continue;
            const bool active_today = series.history[m].contains(day)
                || (raw && std::ranges::any_of(raw->bars(), [&](const auto& bar) {
                       return bar.instrument().exchange() == series.candidates[m].venue
                           && bar.instrument().symbol() == series.candidates[m].symbol;
                   }));
            if (!active_today)
                continue;
            auto oi = prev_open_interest(series, m, day);
            if (!oi && !had_history && series.history[m].contains(day))
                oi = series.history[m].at(day).open_interest;
            if (!oi)
                continue;
            if (!chosen || *oi > best_oi) {
                chosen = m;
                best_oi = *oi;
            }
        }
        return chosen;
    }

    static std::optional<std::size_t> choose_sub_dominant(
        const LiveSeries& series,
        const std::string& day,
        const wire::Bars* raw = nullptr)
    {
        auto is_active = [&](std::size_t m) {
            return series.history[m].contains(day)
                || (raw && std::ranges::any_of(raw->bars(), [&](const auto& bar) {
                       return bar.instrument().exchange() == series.candidates[m].venue
                           && bar.instrument().symbol() == series.candidates[m].symbol;
                   }));
        };
        bool any_prev = false;
        for (std::size_t m = series.active_index + 1; m < series.candidates.size(); ++m)
            if (is_active(m) && prev_open_interest(series, m, day)) {
                any_prev = true;
                break;
            }
        std::optional<std::size_t> chosen;
        Decimal best_oi;
        for (std::size_t m = series.active_index + 1; m < series.candidates.size(); ++m) {
            if (!is_active(m))
                continue;
            auto oi = prev_open_interest(series, m, day);
            if (!oi && !any_prev && series.history[m].contains(day))
                oi = series.history[m].at(day).open_interest;
            if (!oi)
                continue;
            if (!chosen || *oi > best_oi) {
                chosen = m;
                best_oi = *oi;
            }
        }
        return chosen;
    }

    wire::Bars filter_and_roll_bars(const wire::Bars& raw)
    {
        for (std::size_t s = 0; s < live_series.size(); ++s) {
            auto& series = live_series[s];
            std::string day;
            for (const auto& bar : raw.bars()) {
                const InstrumentId id{bar.instrument().exchange(), bar.instrument().symbol()};
                const auto found = std::ranges::find(series.candidates, id);
                if (found != series.candidates.end()
                    && static_cast<std::size_t>(found - series.candidates.begin())
                        >= series.active_index) {
                    day = bar.trading_day();
                    break;
                }
            }
            if (day.empty() || day <= series.decided_day)
                continue;
            const bool had_history = !series.decided_day.empty();
            const auto chosen = choose_dominant(series, day, had_history, &raw);
            if (chosen && *chosen > series.active_index) {
                std::optional<Decimal> ratio;
                if (had_history)
                    ratio = roll_ratio(series, *chosen, day);
                account->roll_strategy_series(authority, s, series.candidates[*chosen]).get();
                const auto old_symbol = series.candidates[series.active_index].symbol;
                series.active_index = *chosen;
                series.pending_roll_ratio = ratio;
                bars->set_awaited(awaited_contracts(day));
                if (ratio)
                    record_event(
                        pb::INSTANCE_EVENT_KIND_UNSPECIFIED,
                        "rolled " + series.symbol + " from " + old_symbol + " to "
                            + series.candidates[*chosen].symbol + " ratio=" + ratio->str());
            }
            series.decided_day = day;
        }
        wire::Bars out;
        for (const auto& fixed : instance.instance().instruments()) {
            for (const auto& bar : raw.bars())
                if (bar.instrument().exchange() == fixed.exchange()
                    && bar.instrument().symbol() == fixed.symbol())
                    *out.add_bars() = bar;
        }
        for (auto& series : live_series) {
            const auto& active = series.candidates[series.active_index];
            for (const auto& bar : raw.bars()) {
                if (bar.instrument().exchange() == active.venue
                    && bar.instrument().symbol() == active.symbol) {
                    auto* added = out.add_bars();
                    *added = bar;
                    if (series.pending_roll_ratio) {
                        *added->mutable_roll_ratio() =
                            protocol::decimal(*series.pending_roll_ratio);
                        series.pending_roll_ratio.reset();
                    }
                    if (const auto sub = choose_sub_dominant(series, bar.trading_day(), &raw)) {
                        const auto& far = series.candidates[*sub];
                        for (const auto& far_bar : raw.bars()) {
                            if (far_bar.instrument().exchange() == far.venue
                                && far_bar.instrument().symbol() == far.symbol
                                && far_bar.has_close() && !far_bar.close().text().empty()
                                && bar.has_close() && !bar.close().text().empty()) {
                                if (const auto ry = annualized_roll_yield(
                                        Decimal::parse(bar.close().text()),
                                        Decimal::parse(far_bar.close().text()),
                                        series.delivery_months[series.active_index],
                                        series.delivery_months[*sub])) {
                                    *added->mutable_roll_yield() = protocol::decimal(*ry);
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
        return out;
    }

    // `execute` false replays a recorded input: state only, no targets reach the account.
    void decide(
        const wire::Bars& input,
        bool execute = true,
        sqlite::Database::Statement* warmup_insert = nullptr)
    {
        for (const auto& bar : input.bars()) {
            const InstrumentId id{bar.instrument().exchange(), bar.instrument().symbol()};
            for (auto& series : live_series) {
                const auto found = std::ranges::find(series.candidates, id);
                if (found != series.candidates.end()) {
                    series.active_index =
                        static_cast<std::size_t>(found - series.candidates.begin());
                    series.decided_day = bar.trading_day();
                    auto& stats = series.history[series.active_index][bar.trading_day()];
                    if (bar.has_open_interest() && !bar.open_interest().text().empty())
                        stats.open_interest = Decimal::parse(bar.open_interest().text());
                }
            }
        }
        const auto expires_at = steady_now(*clock) + 10s;
        const auto input_bytes = input.SerializeAsString();
        // Commit live inputs before delivering them. A missing reply remains
        // unknown in the record and is never replayed into a live child.
        if (!warmup_insert) {
            sqlite::Database::Transaction transaction(*records);
            sqlite::Database::Statement insert(
                *records,
                "INSERT INTO steps(sequence,input) VALUES(?,?)");
            insert.bind(1, static_cast<std::int64_t>(input.sequence()))
                .bind_blob(2, input_bytes)
                .step();
            transaction.commit();
        }
        wire::Request request;
        *request.mutable_bars() = input;
        channel.send(request.SerializeAsString(), 1s);
        wire::Response reply;
        if (!reply.ParseFromString(channel.receive(2s)) || !reply.has_bars())
            throw std::runtime_error(
                reply.has_error() ? reply.error().message() : "strategy returned no decision");
        const auto& decision = reply.bars();
        if (decision.sequence() != input.sequence()
            || (!decision.ready() && decision.targets_size()))
            throw std::runtime_error("strategy decision sequence or readiness is invalid");
        std::set<InstrumentId> selected_ids, targets;
        for (const auto& id : instance.instance().instruments())
            selected_ids.insert({id.exchange(), id.symbol()});
        for (const auto& series : live_series)
            selected_ids.insert(series.candidates[series.active_index]);
        int mode = 0;
        for (const auto& target : decision.targets()) {
            const InstrumentId id{target.instrument().exchange(), target.instrument().symbol()};
            if (!selected_ids.contains(id) || !targets.insert(id).second || !target.size_case()
                || (mode && mode != target.size_case()))
                throw std::runtime_error("strategy decision has invalid targets");
            mode = target.size_case();
        }
        if (warmup_insert) {
            warmup_insert->reset();
            warmup_insert->bind(1, static_cast<std::int64_t>(input.sequence()))
                .bind_blob(2, input_bytes)
                .bind_blob(3, decision.SerializeAsString())
                .step();
        } else {
            sqlite::Database::Transaction transaction(*records);
            sqlite::Database::Statement update(
                *records,
                "UPDATE steps SET decision=? WHERE sequence=?");
            update.bind_blob(1, decision.SerializeAsString())
                .bind(2, static_cast<std::int64_t>(input.sequence()))
                .step();
            transaction.commit();
        }
        instance.set_completed_steps(input.sequence());
        instance.set_last_bar_ms(input.bars(0).end_ms());
        *instance.mutable_last_decision() = decision;
        instance.set_state(
            decision.ready() ? pb::INSTANCE_STATE_READY : pb::INSTANCE_STATE_WARMING);
        if (!execute)
            return;
        auto exec_decision = decision;
        for (std::size_t s = 0; s < live_series.size(); ++s) {
            const auto& series = live_series[s];
            std::string live_day;
            for (std::size_t m = series.active_index; m < series.history.size(); ++m)
                if (!series.history[m].empty() && series.history[m].rbegin()->first > live_day)
                    live_day = series.history[m].rbegin()->first;
            if (live_day.empty() || live_day <= series.decided_day)
                continue;
            const auto chosen = choose_dominant(series, live_day, true);
            if (!chosen || *chosen <= series.active_index)
                continue;
            const auto& from = series.candidates[series.active_index];
            const auto& to = series.candidates[*chosen];
            (void)roll_ratio(series, *chosen, live_day);
            account->roll_strategy_series(authority, s, to).get();
            for (auto& target : *exec_decision.mutable_targets())
                if (target.instrument().exchange() == from.venue
                    && target.instrument().symbol() == from.symbol) {
                    target.mutable_instrument()->set_exchange(to.venue);
                    target.mutable_instrument()->set_symbol(to.symbol);
                }
        }
        account->accept_strategy_target(authority, exec_decision, latest_quotes(), expires_at)
            .get();
        save();
    }

    void advance()
    {
        observe();
        if (!feed)
            return;
        try {
            // The account detects execution faults independently of the child
            // and has already fenced sends and scheduled its cancellations.
            if (authority) {
                if (account->strategy_has_failure()) {
                    const auto failure = account->strategy_failure(authority).get();
                    if (!failure.empty())
                        throw std::runtime_error(failure);
                }
                if (account->strategy_events_pending()) {
                    for (const auto& event : account->take_strategy_events(authority).get())
                        record_event(event.kind(), event.text());
                }
            }
            const auto now = steady_now(*clock);
            if (next_feed_reconnect != std::chrono::steady_clock::time_point{}
                && now >= next_feed_reconnect) {
                next_feed_reconnect = {};
                feed->connect(market, subscribed_contracts);
            }
            const auto update = feed->poll();
            if (update.failed)
                throw std::runtime_error("strategy market stream lost observations");
            bool quotes_changed = false;
            for (const auto& event : update.events) {
                if (const auto* status = std::get_if<ctp::FeedStatus>(&event)) {
                    // CTP reconnects by itself; the window continues across the gap.
                    if (status->phase == MarketPhase::error) {
                        if (!connected && status->error_code != 7 && status->error_code != 8)
                            throw std::runtime_error("strategy market connection failed");
                        log::warn(
                            "strategy {} market feed error code={}; reconnecting in 5s",
                            instance.instance().id(),
                            status->error_code);
                        if (online) {
                            online = false;
                            bars->interrupt();
                            record_event(
                                pb::INSTANCE_EVENT_KIND_MARKET_DISCONNECTED,
                                "market disconnected");
                        }
                        feed->disconnect();
                        next_feed_reconnect = now + 5s;
                    } else if (status->phase == MarketPhase::connected && !online) {
                        if (connected)
                            record_event(
                                pb::INSTANCE_EVENT_KIND_MARKET_RECONNECTED,
                                "market reconnected");
                        connected = online = true;
                        next_feed_reconnect = {};
                        if (feed->failed_subscriptions() > 0)
                            next_subscription_retry = now + 5s;
                    } else if (status->phase != MarketPhase::connected && online) {
                        online = false;
                        bars->interrupt();
                        record_event(
                            pb::INSTANCE_EVENT_KIND_MARKET_DISCONNECTED,
                            "market disconnected");
                    }
                } else if (const auto* subscription = std::get_if<MarketSubscription>(&event)) {
                    if (subscription->state == SubscriptionState::error) {
                        log::warn(
                            "strategy {} subscription rejected: {}/{} code={}; will retry",
                            instance.instance().id(),
                            subscription->instrument.venue,
                            subscription->instrument.symbol,
                            subscription->error_code);
                        if (next_subscription_retry == std::chrono::steady_clock::time_point{})
                            next_subscription_retry = now + 5s;
                    }
                } else if (const auto* quote = std::get_if<MarketQuoteObservation>(&event)) {
                    if (quote->out_of_order)
                        continue;
                    const auto& observed = quote->quote;
                    const auto held = quotes.find(observed.instrument);
                    if (observed.received_ms - observed.source_ms <= 10000
                        && observed.source_ms - observed.received_ms <= 2000
                        && (held == quotes.end() || observed.source_ms >= held->second.source_ms)) {
                        quotes.insert_or_assign(observed.instrument, observed);
                        quotes_changed = true;
                        observe_series_quote(observed);
                    }
                    for (const auto& input : bars->observe(observed)) {
                        store_local_bars(instance.instance().period(), input);
                        if (live_series.empty()) {
                            decide(input);
                            quotes_changed = true;
                        } else {
                            auto filtered = filter_and_roll_bars(input);
                            if (filtered.bars_size() > 0) {
                                filtered.set_sequence(++live_sequence);
                                decide(filtered);
                                quotes_changed = true;
                            }
                        }
                    }
                }
            }
            if (online && feed->failed_subscriptions() > 0
                && next_subscription_retry != std::chrono::steady_clock::time_point{}
                && now >= next_subscription_retry) {
                feed->retry_failed_subscriptions();
                next_subscription_retry = now + 5s;
            }
            if (!connected && now >= connect_deadline)
                throw std::runtime_error("strategy market login timed out");
            // Process all queued feed faults before admitting another leg. SDK
            // and journal waits run on the account loop, never on this worker.
            if (execution.valid()) {
                if (execution.wait_for(0s) != std::future_status::ready)
                    return;
                execution.get();
            }
            if (quotes_changed || account->strategy_batch_active()
                || now >= next_target_heartbeat) {
                execution = account->advance_strategy_target(authority, latest_quotes());
                next_target_heartbeat = now + 500ms;
            }
        } catch (const std::exception& error) {
            instance.set_state(pb::INSTANCE_STATE_FAILED);
            instance.set_error(error.what());
            instance.set_process_id(0);
            close();
            save();
        }
    }

    void observe()
    {
        if (child && child->exited()) {
            instance.set_state(pb::INSTANCE_STATE_FAILED);
            instance.set_error(
                "strategy process exited with code " + std::to_string(child->exit_code()));
            instance.set_process_id(0);
            close();
            save();
        }
    }

    pb::Strategies snapshot()
    {
        observe();
        pb::Strategies result;
        for (const auto& name : fs::directory_iterator(root / "packages")) {
            if (!name.is_directory() || name.path().filename().string().starts_with('.'))
                continue;
            for (const auto& version : fs::directory_iterator(name.path())) {
                if (version.path().filename().string().starts_with('.'))
                    continue;
                auto* manifest = result.add_packages();
                if (!google::protobuf::util::JsonStringToMessage(
                        read(version.path() / "strategy.json", 16384),
                        manifest)
                        .ok())
                    throw std::invalid_argument(
                        "invalid installed strategy manifest: " + version.path().string());
            }
        }
        if (instance.has_instance())
            *result.mutable_instance() = instance;
        return result;
    }

    pb::RecordedBars recorded_bars(const pb::GetRecordedBars& query)
    {
        if (query.period() == ::common::PERIOD_UNSPECIFIED || query.period() > ::common::PERIOD_DAY)
            throw std::invalid_argument("recorded bars require a valid period");
        const auto limit = query.limit() ? query.limit() : 500U;
        if (limit > 500U)
            throw std::invalid_argument("recorded bar pages contain 1 to 500 bars");
        const auto& exchange = query.instrument().exchange();
        const auto& symbol = query.instrument().symbol();
        sqlite::Database::Statement statement(
            *local_bars,
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

    pb::StrategyRun strategy_run(const pb::GetStrategyRun& query)
    {
        const auto id = !query.id().empty() ? query.id()
            : instance.has_instance()       ? instance.instance().id()
                                            : std::string{};
        if (id.empty())
            throw std::invalid_argument("no strategy instance run is recorded");
        package_identity(id, "instance");
        const auto limit = query.limit() ? query.limit() : 200U;
        if (limit > 500U)
            throw std::invalid_argument("strategy run pages contain 1 to 500 steps");
        std::optional<sqlite::Database> opened;
        sqlite::Database* db = nullptr;
        if (records && instance.has_instance() && id == instance.instance().id()) {
            db = records.get();
        } else {
            const auto run_file = root / "runs" / (id + ".sqlite");
            if (!fs::is_regular_file(run_file))
                throw std::invalid_argument("no recorded strategy instance: " + id);
            opened.emplace(run_file, sqlite::Database::Access::read_only);
            db = &*opened;
        }
        pb::StrategyRun out;
        {
            sqlite::Database::Statement cfg(*db, "SELECT value FROM configuration");
            if (!cfg.step() || !out.mutable_configuration()->ParseFromString(cfg.blob(0)))
                throw std::runtime_error("invalid strategy configuration record: " + id);
        }
        {
            sqlite::Database::Statement steps(
                *db,
                "SELECT input, decision IS NOT NULL, decision FROM steps " "WHERE sequence > ? ORDER BY sequence ASC LIMIT ?");
            steps.bind(1, static_cast<std::int64_t>(query.after_sequence()))
                .bind(2, static_cast<std::int64_t>(limit));
            while (steps.step()) {
                auto* step = out.add_steps();
                if (!step->mutable_input()->ParseFromString(steps.blob(0)))
                    throw std::runtime_error("invalid strategy step input record: " + id);
                if (steps.integer(1) && !step->mutable_decision()->ParseFromString(steps.blob(2)))
                    throw std::runtime_error("invalid strategy step decision record: " + id);
            }
        }
        {
            sqlite::Database::Statement events(
                *db,
                "SELECT time_ms, kind, event FROM events ORDER BY rowid ASC");
            while (events.step()) {
                auto* item = out.add_events();
                item->set_time_ms(events.integer(0));
                item->set_kind(static_cast<pb::InstanceEventKind>(events.integer(1)));
                item->set_text(events.text(2));
            }
        }
        return out;
    }

    void publish(const wire::Package& package)
    {
        validate(package);
        const auto& manifest = package.manifest();
        const auto parent = root / "packages" / manifest.name();
        create_directories_durably(parent);
        const auto destination = parent / manifest.version();
        if (fs::exists(destination))
            throw Error(
                ErrorCode::conflict,
                "strategy version is already installed; publish a new version");
        const auto temporary = parent / (".publish-" + unique_process_id());
        try {
            create_directories_durably(temporary);
            for (const auto& file : package.files()) {
                const auto path = temporary / file.path();
                create_directories_durably(path.parent_path());
                write_file_durably(path, file.content());
            }
            write_file_durably(temporary / "strategy.json", json(manifest));
            fs::rename(temporary, destination);
            sync_directory(parent);
        } catch (...) {
            fs::remove_all(temporary);
            throw;
        }
    }

    void start(const wire::Instance& configuration, LiveSession* execution)
    {
        observe();
        if (child)
            throw Error(ErrorCode::conflict, "stop the loaded strategy before starting another");
        validate(configuration);
        if (!execution)
            throw Error(
                ErrorCode::unavailable,
                "initialize and synchronize the account before starting a strategy");
        const auto package =
            root / "packages" / configuration.package_name() / configuration.package_version();
        (void)read(package / "strategy.json", 16384);
        const auto run = root / "runs" / (configuration.id() + ".sqlite");
        if (fs::exists(run))
            throw Error(ErrorCode::conflict, "strategy instance id already has a recorded run");
        const auto expiries = cached_expiries(root);
        std::map<std::int64_t, wire::Bars> merged;
        if (configuration.has_warmup())
            for (const auto& batch : configuration.warmup().bars())
                merged.emplace(batch.bars(0).end_ms(), batch);
        if (configuration.local_bars() || !configuration.resume_from().empty()) {
            const auto resumed = configuration.local_bars()
                ? load_local_bars(*local_bars, configuration, configuration.has_warmup(), expiries)
                : recorded_inputs(
                      root / "runs" / (configuration.resume_from() + ".sqlite"),
                      configuration);
            for (const auto& batch : resumed)
                merged.try_emplace(batch.bars(0).end_ms(), batch);
        }
        std::vector<wire::Bars> replay;
        replay.reserve(merged.size());
        for (auto& [_, batch] : merged)
            replay.push_back(std::move(batch));
        const auto python = installation / "runtime/python/bin/python3";
        const auto code = installation / "lib/strategy";
        if (!fs::is_regular_file(python) || !fs::is_regular_file(code / "strategy_runner.py"))
            throw Error(
                ErrorCode::unavailable,
                "strategy runtime must be installed at " + python.string() + " with code in "
                    + code.string());
        std::vector<InstrumentId> contracts, subscribed;
        for (const auto& id : configuration.instruments()) {
            contracts.push_back({id.exchange(), id.symbol()});
            subscribed.push_back(contracts.back());
        }
        std::vector<DominantSeriesSpec> series_specs;
        live_series.clear();
        for (const auto& series : configuration.series()) {
            DominantSeriesSpec spec;
            spec.symbol = series_symbol(series);
            LiveSeries state;
            for (const auto& entry : series.contracts()) {
                const InstrumentId id{entry.instrument().exchange(), entry.instrument().symbol()};
                spec.candidates.push_back(id);
                state.delivery_months.push_back(entry.delivery_month());
                const auto exp = expiries.find(id);
                state.expiry_days.push_back(exp == expiries.end() ? std::string{} : exp->second);
            }
            for (auto it = replay.rbegin(); it != replay.rend(); ++it) {
                bool matched = false;
                for (const auto& bar : it->bars()) {
                    const InstrumentId id{bar.instrument().exchange(), bar.instrument().symbol()};
                    const auto found = std::ranges::find(spec.candidates, id);
                    if (found != spec.candidates.end()) {
                        spec.initial_index =
                            static_cast<std::size_t>(found - spec.candidates.begin());
                        matched = true;
                        break;
                    }
                }
                if (matched)
                    break;
            }
            for (std::size_t c = spec.initial_index; c < spec.candidates.size(); ++c)
                subscribed.push_back(spec.candidates[c]);
            state.symbol = spec.symbol;
            state.candidates = spec.candidates;
            state.active_index = spec.initial_index;
            state.history.resize(spec.candidates.size());
            live_series.push_back(std::move(state));
            series_specs.push_back(std::move(spec));
        }
        std::optional<StrategyWeightBudget> budget;
        if (configuration.has_weight_limits()) {
            const auto& wl = configuration.weight_limits();
            budget = StrategyWeightBudget{
                Decimal::parse(wl.gross().text()),
                Decimal::parse(wl.net().text()),
                wl.has_max_margin_ratio() && !wl.max_margin_ratio().text().empty()
                    ? Decimal::parse(wl.max_margin_ratio().text())
                    : Decimal{},
                wl.has_min_rebalance_weight() && !wl.min_rebalance_weight().text().empty()
                    ? Decimal::parse(wl.min_rebalance_weight().text())
                    : Decimal{}
            };
        }
        authority =
            execution
                ->register_strategy(configuration.id(), contracts, budget, std::move(series_specs))
                .get();
        account = execution;
        auto persisted = configuration;
        if (persisted.has_warmup())
            persisted.mutable_warmup()->clear_bars();
        instance.Clear();
        *instance.mutable_instance() = persisted;
        instance.set_state(pb::INSTANCE_STATE_STARTING);
        try {
            save();
            create_directories_durably(root / "runs");
            records = std::make_unique<sqlite::Database>(run);
            records->execute("CREATE TABLE configuration (value BLOB NOT NULL)");
            records->execute(
                "CREATE TABLE steps (sequence INTEGER PRIMARY KEY, input BLOB NOT NULL, decision BLOB)");
            records->execute(
                "CREATE TABLE events (time_ms INTEGER NOT NULL, kind INTEGER NOT NULL, event TEXT NOT NULL)");
            {
                sqlite::Database::Transaction transaction(*records);
                sqlite::Database::Statement insert(*records, "INSERT INTO configuration VALUES(?)");
                insert.bind_blob(1, persisted.SerializeAsString()).step();
                transaction.commit();
            }
            if (configuration.has_warmup()) {
                record_event(
                    pb::INSTANCE_EVENT_KIND_UNSPECIFIED,
                    "warmup dataset_version=" + configuration.warmup().dataset_version()
                        + " sha256=" + configuration.warmup().sha256());
            }
            sync_directory(run.parent_path());
            const auto directory =
                fs::temp_directory_path() / ("ast-strategy-" + unique_process_id().substr(0, 16));
            fs::create_directory(directory);
            fs::permissions(directory, fs::perms::owner_all);
            endpoint = directory / "child.sock";
            const auto bootstrap = "import sys; sys.path.insert(0, " + json_string(code.string())
                + "); import strategy_runner; strategy_runner.main()";
            const auto diagnostics =
                installation / "logs" / ("strategy-" + unique_process_id() + ".log");
            create_directories_durably(diagnostics.parent_path());
            log::info("strategy {} diagnostics: {}", configuration.id(), diagnostics.string());
            child = std::make_unique<ChildProcess>(
                python,
                std::vector<std::string>{
                    "-I",
                    "-B",
                    "-X",
                    "faulthandler",
                    "-c",
                    bootstrap,
                    endpoint.string(),
                    package.string(),
                    std::to_string(::getpid())
                },
                true,
                diagnostics,
                true);
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (!fs::exists(endpoint)) {
                if (child->exited())
                    throw std::runtime_error("strategy runtime exited before opening its socket");
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error(
                        "strategy runtime did not open its socket within 5 seconds");
                std::this_thread::sleep_for(10ms);
            }
            channel = ipc::Channel::connect(endpoint.string(), 1s);
            wire::Request request;
            *request.mutable_load()->mutable_instance() = persisted;
            for (const auto& symbol : authority->symbols())
                request.mutable_load()->add_symbols(symbol);
            channel.send(request.SerializeAsString(), 1s);
            wire::Response reply;
            if (!reply.ParseFromString(channel.receive(10s)) || !reply.has_load())
                throw std::invalid_argument(
                    reply.has_error() ? reply.error().message() : "strategy did not load");
            std::uint64_t sequence = 0;
            if (!replay.empty()) {
                sqlite::Database::Transaction warmup_tx(*records);
                sqlite::Database::Statement warmup_insert(
                    *records,
                    "INSERT INTO steps(sequence,input,decision) VALUES(?,?,?)");
                for (auto input : replay) {
                    input.set_sequence(++sequence);
                    decide(input, false, &warmup_insert);
                }
                warmup_tx.commit();
            }
            live_sequence = sequence;
            bars = std::make_unique<StrategyBars>(
                persisted,
                clock,
                sequence,
                replay.empty() ? 0 : replay.back().bars(0).end_ms());
            bars->set_awaited(awaited_contracts());
            feed = std::make_unique<ctp::Feed>(
                sdk_owner,
                installation / "lib/ctp-md.so",
                root / "ctp-flow");
            connected = online = false;
            subscribed_contracts = subscribed;
            next_subscription_retry = {};
            next_feed_reconnect = {};
            next_target_heartbeat = {};
            last_awaited_day.clear();
            connect_deadline = steady_now(*clock) + 30s;
            feed->connect(market, subscribed_contracts);
            running.store(true, std::memory_order_release);
            if (replay.empty())
                instance.set_state(pb::INSTANCE_STATE_WARMING);
            instance.set_process_id(child->id());
            save();
        } catch (const std::exception& error) {
            close();
            instance.set_state(pb::INSTANCE_STATE_FAILED);
            instance.set_process_id(0);
            instance.set_error(error.what());
            save();
            throw;
        }
    }

    static std::string json_string(const std::string& value)
    {
        // JSON string syntax is also a Python string literal; paths are data,
        // never interpolated as executable Python or passed through a shell.
        return nlohmann::json(value).dump();
    }

    void stop()
    {
        if (!child && !authority)
            return;
        close();
        instance.set_state(pb::INSTANCE_STATE_STOPPED);
        instance.set_process_id(0);
        save();
    }

    fs::path installation, root, endpoint;
    std::unique_ptr<FileLock> lock;
    pb::InstanceStatus instance;
    std::unique_ptr<ChildProcess> child;
    ipc::Channel channel;
    ctp::Configuration market;
    ThreadPool sdk_owner{1, 8};
    std::unique_ptr<ctp::Feed> feed;
    std::unique_ptr<StrategyBars> bars;
    std::unique_ptr<sqlite::Database> records;
    std::unique_ptr<sqlite::Database> local_bars;
    LiveSession* account = nullptr;
    StrategyAuthorityPtr authority;
    std::map<InstrumentId, MarketQuote> quotes;
    std::future<void> execution;
    bool connected = false, online = false;
    std::chrono::steady_clock::time_point connect_deadline;
    std::shared_ptr<AlertWebhook> alerts;
    std::uint64_t abandoned_batches = 0;
    std::vector<LiveSeries> live_series;
    std::uint64_t live_sequence = 0;
    std::vector<InstrumentId> subscribed_contracts;
    std::chrono::steady_clock::time_point next_subscription_retry{}, next_feed_reconnect{};
    std::shared_ptr<const Clock> clock;
    std::map<std::tuple<int, std::string, std::string>, std::uint64_t> bar_write_counts;
    std::atomic<bool> running{false};
    std::string last_awaited_day;
    std::chrono::steady_clock::time_point next_target_heartbeat{};
};

StrategyHost::StrategyHost(
    fs::path installation,
    fs::path account,
    ctp::Configuration market,
    std::shared_ptr<AlertWebhook> alerts,
    std::shared_ptr<const Clock> clock)
    : state_(
          std::make_unique<State>(
              std::move(installation),
              std::move(account),
              std::move(market),
              std::move(alerts),
              std::move(clock)))
{}

StrategyHost::~StrategyHost()
{
    catalog_stop_.request_stop();
    catalog_worker_.shutdown();
    worker_.shutdown();
}

std::future<pb::RecordedBars> StrategyHost::recorded_bars(pb::GetRecordedBars query)
{
    if (closing_)
        throw Error(ErrorCode::unavailable, "strategy host is stopping");
    auto result = std::make_shared<std::promise<pb::RecordedBars>>();
    auto future = result->get_future();
    (void)worker_.submit([this, query = std::move(query), result](std::stop_token) {
        try {
            result->set_value(state_->recorded_bars(query));
        } catch (...) {
            result->set_exception(std::current_exception());
        }
    });
    return future;
}

std::future<pb::StrategyRun> StrategyHost::strategy_run(pb::GetStrategyRun query)
{
    if (closing_)
        throw Error(ErrorCode::unavailable, "strategy host is stopping");
    auto result = std::make_shared<std::promise<pb::StrategyRun>>();
    auto future = result->get_future();
    (void)worker_.submit([this, query = std::move(query), result](std::stop_token) {
        try {
            result->set_value(state_->strategy_run(query));
        } catch (...) {
            result->set_exception(std::current_exception());
        }
    });
    return future;
}

std::future<pb::ContractCatalog> StrategyHost::catalog(
    pb::GetCatalog query,
    fs::path ctp_library,
    ctp::CatalogConfiguration configuration)
{
    if (closing_)
        throw Error(ErrorCode::unavailable, "strategy host is stopping");
    auto result = std::make_shared<std::promise<pb::ContractCatalog>>();
    auto future = result->get_future();
    const auto root = state_->root;
    const auto stop = catalog_stop_.get_token();
    (void)catalog_worker_.submit([query = std::move(query),
                                     ctp_library = std::move(ctp_library),
                                     configuration = std::move(configuration),
                                     root,
                                     stop,
                                     result](std::stop_token) mutable {
        try {
            const auto cache = root / "catalog.pb";
            if (!query.refresh() && fs::is_regular_file(cache) && !fs::is_symlink(cache)
                && fs::file_size(cache) <= 64 * 1024 * 1024) {
                std::ifstream input(cache, std::ios::binary);
                pb::ContractCatalog cached;
                if (cached.ParseFromIstream(&input) && cached.contracts_size() > 0
                    && cached.contracts_size() <= 20000) {
                    result->set_value(std::move(cached));
                    return;
                }
            }
            ctp::CatalogQuery session(ctp_library, root / "catalog-flow", std::move(configuration));
            for (;;) {
                if (auto fetched = session.poll(stop)) {
                    pb::ContractCatalog out;
                    out.set_trading_day(protocol::iso_day(fetched->trading_day));
                    for (const auto& entry : fetched->contracts) {
                        auto* item = out.add_contracts();
                        item->mutable_instrument()->set_exchange(entry.instrument.venue);
                        item->mutable_instrument()->set_symbol(entry.instrument.symbol);
                        item->set_contract(entry.contract_id);
                        item->set_product(entry.product);
                        item->set_name(entry.name);
                        item->set_expiry_day(protocol::iso_day(entry.expiry));
                        item->set_multiplier(entry.multiplier);
                        *item->mutable_price_tick() = protocol::decimal(entry.price_tick);
                    }
                    try {
                        replace_file_durably(cache, out.SerializeAsString());
                    } catch (const std::exception& error) {
                        log::warn(
                            "trading contract catalog cache could not be saved: {}",
                            error.what());
                    }
                    result->set_value(std::move(out));
                    return;
                }
                std::this_thread::sleep_for(20ms);
            }
        } catch (...) {
            result->set_exception(std::current_exception());
        }
    });
    return future;
}

std::future<pb::Strategies> StrategyHost::request(pb::Request request, LiveSession* account)
{
    if (closing_)
        throw Error(ErrorCode::unavailable, "strategy host is stopping");
    auto result = std::make_shared<std::promise<pb::Strategies>>();
    auto future = result->get_future();
    (void)worker_.submit([this, request = std::move(request), result, account](std::stop_token) {
        try {
            if (request.has_publish_strategy())
                state_->publish(request.publish_strategy());
            else if (request.has_start_strategy())
                state_->start(request.start_strategy(), account);
            else if (request.has_pause_strategy())
                state_->pause(request.pause_strategy());
            else if (request.has_flatten_strategy())
                state_->flatten(request.flatten_strategy());
            else if (request.has_stop_strategy()) {
                if (request.stop_strategy().id().empty())
                    throw std::invalid_argument("stopping a strategy requires its instance id");
                if (!state_->instance.has_instance()
                    || state_->instance.instance().id() != request.stop_strategy().id())
                    throw Error(
                        ErrorCode::conflict,
                        "strategy instance changed; inspect before stopping");
                state_->stop();
            } else if (!request.has_get_strategies())
                throw std::invalid_argument("missing strategy operation");
            result->set_value(state_->snapshot());
        } catch (...) {
            result->set_exception(std::current_exception());
        }
    });
    return future;
}

void StrategyHost::poll()
{
    if (closing_ || !state_->running.load(std::memory_order_acquire))
        return;
    if (observation_.valid()) {
        if (observation_.wait_for(0s) != std::future_status::ready)
            return;
        try {
            observation_.get();
        } catch (const std::exception& error) {
            log::error("strategy status persistence failed: {}", error.what());
        }
    }
    try {
        observation_ = worker_.submit([this](std::stop_token) {
            state_->advance();
        });
    } catch (const Error& error) {
        if (error.code() != ErrorCode::resource_exhausted)
            throw;
        // Accepted administration filled the bounded queue. Observe exit on
        // the next owner turn; client traffic must not terminate the account.
    }
}

void StrategyHost::stop()
{
    if (shutdown_.valid())
        return;
    closing_ = true;
    catalog_stop_.request_stop();
    try {
        shutdown_ = worker_.submit([this](std::stop_token) {
            state_->stop();
        });
    } catch (const Error& error) {
        if (error.code() != ErrorCode::resource_exhausted)
            throw;
        // Admission is closed; enqueue shutdown once accepted work advances.
    }
}

bool StrategyHost::stopped() const
{
    return shutdown_.valid() && shutdown_.wait_for(0s) == std::future_status::ready;
}

} // namespace asterion::trading
