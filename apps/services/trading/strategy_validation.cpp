#include "strategy_validation.hpp"
#include "strategy_execution.hpp"
#include <asterion/futures/futures.hpp>
#include <asterion/futures/history_identity.hpp>
#include <asterion/system/process/artifact.hpp>
#include <google/protobuf/util/json_util.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <regex>
#include <set>

namespace asterion::trading {
namespace fs = std::filesystem;
namespace wire = ::strategy;

namespace {

int delivery_months(const std::string& month)
{
    const auto dash = month.find('-');
    return std::stoi(month.substr(0, dash)) * 12 + std::stoi(month.substr(dash + 1));
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

} // namespace

std::string read_strategy_file(const fs::path& path, std::size_t limit)
{
    if (!fs::is_regular_file(path) || fs::is_symlink(path) || fs::file_size(path) > limit)
        throw std::invalid_argument("missing or invalid strategy file: " + path.string());
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

std::string proto_to_json(const google::protobuf::Message& message)
{
    std::string result;
    if (!google::protobuf::util::MessageToJsonString(message, &result).ok())
        throw std::runtime_error("cannot encode strategy description");
    return result;
}

void validate_package_identity(const std::string& name, const std::string& version)
{
    const std::regex component("[A-Za-z0-9][A-Za-z0-9._-]{0,63}");
    if (!std::regex_match(name, component) || !std::regex_match(version, component))
        throw std::invalid_argument(
            "package name and version must be 1-64 letters, digits, dots, underscores or hyphens, beginning with a letter or digit");
}

void validate_strategy_package(const wire::Package& package)
{
    const auto& m = package.manifest();
    validate_package_identity(m.name(), m.version());
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

std::optional<Decimal> simple_return(Decimal current, Decimal previous)
{
    if (current <= Decimal{} || previous <= Decimal{})
        return std::nullopt;
    return divide(current - previous, previous, Rounding::half_up);
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
                + (bar.has_roll_yield() ? "|ry=" + bar.roll_yield().text() : "")
                + (bar.has_far_close() ? "|fc=" + bar.far_close().text() : "")
                + (bar.has_near_return() ? "|nr=" + bar.near_return().text() : "")
                + (bar.has_far_return() ? "|fr=" + bar.far_return().text() : "")
                + (bar.has_receipt_volume() ? "|rv=" + bar.receipt_volume().text() : "")
                + (bar.has_member_long() ? "|ml=" + bar.member_long().text() : "")
                + (bar.has_member_short() ? "|ms=" + bar.member_short().text() : "")
                + (bar.has_inventory_total() ? "|inv=" + bar.inventory_total().text() : "") + "\n";
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

void validate_strategy_instance(const wire::Instance& instance)
{
    validate_package_identity(instance.id(), "instance");
    validate_package_identity(instance.package_name(), instance.package_version());
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
    if (instance.has_execution()) {
        (void)normalize_execution_options(
            {instance.execution().aggressive_ticks(),
                instance.execution().max_reprices(),
                instance.execution().reprice_interval_ms(),
                instance.execution().twap_slices(),
                instance.execution().twap_interval_ms()});
    }
    auto stripped = instance;
    if (stripped.has_warmup())
        stripped.mutable_warmup()->clear_bars();
    if (stripped.ByteSizeLong() > 65536 || instance.ByteSizeLong() > 2 * 1024 * 1024)
        throw std::invalid_argument("strategy instance configuration exceeds 64 KiB");
    if (!instance.resume_from().empty()) {
        validate_package_identity(instance.resume_from(), "instance");
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
        validate_package_identity(warmup.dataset_version(), "version");
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
                if (bar.has_far_close())
                    (void)Decimal::parse(bar.far_close().text());
                if (bar.has_near_return())
                    (void)Decimal::parse(bar.near_return().text());
                if (bar.has_far_return())
                    (void)Decimal::parse(bar.far_return().text());
                if (bar.has_receipt_volume())
                    (void)Decimal::parse(bar.receipt_volume().text());
                if (bar.has_member_long())
                    (void)Decimal::parse(bar.member_long().text());
                if (bar.has_member_short())
                    (void)Decimal::parse(bar.member_short().text());
                if (bar.has_inventory_total())
                    (void)Decimal::parse(bar.inventory_total().text());
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

} // namespace asterion::trading
