#include <set>
#include <asterion/futures/futures.hpp>
#include <asterion/futures/history_identity.hpp>
#include <asterion/base/id.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace asterion::protocol {
namespace {

Decimal value(const proto::Decimal& decimal)
{
    return Decimal::from_raw(decimal.units());
}

void set(proto::Decimal* target, Decimal decimal)
{
    target->set_units(decimal.raw());
}

Decimal decimal(const Json& json, const char* key)
{
    return Decimal::parse(json.at(key).get<std::string>());
}

std::int64_t nanoseconds(const Json& json)
{
    const auto text = json.get<std::string>();
    std::int64_t result = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size() || std::to_string(result) != text)
        throw std::invalid_argument("invalid bar timestamp");
    return result;
}

bool digest(const std::string& text)
{
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// None of them means daily bars from the daily table.
void validate_versions(const google::protobuf::RepeatedPtrField<std::string>& ids)
{
    if (ids.size() > max_dataset_sources)
        throw std::invalid_argument("dataset takes at most 32 source versions");
    for (int i = 0; i < ids.size(); ++i)
        if (!digest(ids.Get(i)) || (i && ids.Get(i) <= ids.Get(i - 1)))
            throw std::invalid_argument(
                "dataset versions must be sorted unique SHA-256 identities");
}

void encode_versions(const Json& json, google::protobuf::RepeatedPtrField<std::string>* out)
{
    if (!json.is_array() || json.size() > static_cast<std::size_t>(max_dataset_sources))
        throw std::invalid_argument("dataset takes at most 32 source versions");
    auto ids = json.get<std::vector<std::string>>();
    std::ranges::sort(ids);
    for (const auto& id : ids)
        *out->Add() = id;
    validate_versions(*out);
}

Json versions(const google::protobuf::RepeatedPtrField<std::string>& ids)
{
    return std::vector<std::string>(ids.begin(), ids.end());
}

Json bar_json(const MarketBar& bar)
{
    return {
        {"trading_day", bar.trading_day},
        {"timestamp_ns", std::to_string(bar.timestamp_ns)},
        {"open", bar.open.str()},
        {"high", bar.high.str()},
        {"low", bar.low.str()},
        {"close", bar.close.str()},
        {"volume", bar.volume.str()}
    };
}

Json increment_changes(const data::proto::BarDataset& dataset)
{
    auto changes = Json::array();
    for (const auto& change : dataset.price_increment_changes())
        changes.push_back(
            {{"trading_day", change.trading_day()}, {"value", value(change.value()).str()}});
    return changes;
}

// Canonical content: the fields that define what the bars are, not where they came from.
Json contents(const data::proto::BarDataset& dataset, DatasetView view)
{
    auto contract = decode_contract(dataset.contract());
    Json result{
        {"contract", std::move(contract)},
        {"price_increment_changes", increment_changes(dataset)},
        {"interval_minutes", dataset.interval_minutes()}
    };
    if (view == DatasetView::metadata)
        return result;
    auto bars = Json::array();
    for (const auto& bar : dataset.bars())
        bars.push_back(bar_json(market_bar(bar)));
    auto days = Json::array();
    for (const auto& day : dataset.days())
        days.push_back(
            {{"trading_day", day.trading_day()},
                {"settlement_price", value(day.settlement_price()).str()}});
    result["bars"] = std::move(bars);
    result["days"] = std::move(days);
    return result;
}

} // namespace

void validate_history_evidence(const data::proto::HistoryVersionEvidence& evidence)
{
    validate_message(evidence);
    if (!digest(evidence.dataset_id()) || evidence.acquired_at_ns() <= 0
        || evidence.source_availability() != data::proto::SOURCE_AVAILABILITY_UNKNOWN)
        throw std::invalid_argument("invalid historical availability evidence");
}

Json decode_history_evidence(const data::proto::HistoryVersionEvidence& evidence)
{
    validate_history_evidence(evidence);
    return {
        {"dataset_id", evidence.dataset_id()},
        {"acquired_at_ns", std::to_string(evidence.acquired_at_ns())},
        {"source_availability", "unknown"}
    };
}

data::proto::HistoryVersionEvidence encode_history_evidence(const Json& json)
{
    require_fields(json, {"dataset_id", "acquired_at_ns", "source_availability"});
    if (json.at("source_availability") != "unknown")
        throw std::invalid_argument("invalid historical availability evidence");
    data::proto::HistoryVersionEvidence result;
    result.set_dataset_id(json.at("dataset_id").get<std::string>());
    result.set_acquired_at_ns(nanoseconds(json.at("acquired_at_ns")));
    result.set_source_availability(data::proto::SOURCE_AVAILABILITY_UNKNOWN);
    validate_history_evidence(result);
    return result;
}

HistoryIdentity history_identity(const proto::Contract& contract)
{
    auto product = contract.product();
    std::ranges::transform(product, product.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    HistoryIdentity identity{contract.venue(), product, contract.delivery_month()};
    identity.validate();
    return identity;
}

Instrument instrument(const proto::Contract& c)
{
    if (!c.has_price_increment() || !c.has_quantity_increment() || !c.has_multiplier())
        throw std::invalid_argument("incomplete contract specification");
    Instrument result{
        {c.venue(), c.symbol()},
        c.currency(),
        value(c.price_increment()),
        value(c.quantity_increment()),
        value(c.multiplier())
    };
    FuturesContract{result, c.product(), c.delivery_month()}.validate();
    return result;
}

proto::Bar encode_bar(const MarketBar& bar)
{
    proto::Bar result;
    result.set_trading_day(bar.trading_day);
    result.set_timestamp_ns(bar.timestamp_ns);
    set(result.mutable_open(), bar.open);
    set(result.mutable_high(), bar.high);
    set(result.mutable_low(), bar.low);
    set(result.mutable_close(), bar.close);
    set(result.mutable_volume(), bar.volume);
    return result;
}

MarketBar market_bar(const proto::Bar& bar)
{
    if (!bar.has_open() || !bar.has_high() || !bar.has_low() || !bar.has_close()
        || !bar.has_volume())
        throw std::invalid_argument("incomplete bar");
    return {
        bar.trading_day(),
        bar.timestamp_ns(),
        value(bar.open()),
        value(bar.high()),
        value(bar.low()),
        value(bar.close()),
        value(bar.volume())
    };
}

std::string bar_dataset_revision(const data::proto::BarDataset& dataset)
{
    // Preserve the exact sorted-key JSON digest without constructing a JSON
    // object for every bar. Only the canonical byte buffer grows with the input.
    std::string canonical = "{\"bars\":[";
    std::string previous_day, encoded_day;
    bool first = true;
    for (const auto& row : dataset.bars()) {
        const auto bar = market_bar(row);
        if (!first)
            canonical += ',';
        if (first || previous_day != bar.trading_day) {
            previous_day = bar.trading_day;
            encoded_day = Json(previous_day).dump();
        }
        first = false;
        canonical += "{\"close\":\"";
        canonical += bar.close.str();
        canonical += "\",\"high\":\"";
        canonical += bar.high.str();
        canonical += "\",\"low\":\"";
        canonical += bar.low.str();
        canonical += "\",\"open\":\"";
        canonical += bar.open.str();
        canonical += "\",\"timestamp_ns\":\"";
        canonical += std::to_string(bar.timestamp_ns);
        canonical += "\",\"trading_day\":";
        canonical += encoded_day;
        canonical += ",\"volume\":\"";
        canonical += bar.volume.str();
        canonical += "\"}";
    }
    canonical += "],\"contract\":";
    canonical += decode_contract(dataset.contract()).dump();
    canonical += ",\"days\":[";
    first = true;
    for (const auto& day : dataset.days()) {
        if (!first)
            canonical += ',';
        first = false;
        canonical += "{\"settlement_price\":\"";
        canonical += value(day.settlement_price()).str();
        canonical += "\",\"trading_day\":";
        canonical += Json(day.trading_day()).dump();
        canonical += '}';
    }
    canonical += "],\"interval_minutes\":";
    canonical += std::to_string(dataset.interval_minutes());
    canonical += ",\"price_increment_changes\":";
    canonical += increment_changes(dataset).dump();
    canonical += "}";
    return sha256_bytes(canonical);
}

std::string format_utc_nanoseconds(std::int64_t value)
{
    using namespace std::chrono;
    const std::chrono::sys_time<std::chrono::nanoseconds> point{std::chrono::nanoseconds{value}};
    const auto day = std::chrono::floor<std::chrono::days>(point);
    const hh_mm_ss clock{point - day};
    std::ostringstream out;
    out << format_trading_date(year_month_day{day}) << 'T' << std::setfill('0') << std::setw(2)
        << clock.hours().count() << ':' << std::setw(2) << clock.minutes().count() << ':'
        << std::setw(2) << clock.seconds().count() << '.' << std::setw(9)
        << clock.subseconds().count() << 'Z';
    return out.str();
}

void validate_bar_dataset(const data::proto::BarDataset& dataset)
{
    validate_message(dataset);
    if (!dataset.has_contract() || dataset.bars_size() < 1
        || static_cast<std::size_t>(dataset.bars_size()) > max_dataset_bars
        || dataset.days_size() < 1
        || static_cast<std::size_t>(dataset.days_size()) > max_dataset_bars)
        throw std::invalid_argument("bar dataset requires 1..200000 bars and their trading days");
    const auto interval = dataset.interval_minutes();
    if (interval > 1440)
        throw std::invalid_argument("unsupported bar interval");
    if (dataset.source().empty() || dataset.source().size() > 64)
        throw std::invalid_argument("invalid bar dataset provenance");
    validate_versions(dataset.source_dataset_ids());
    if (dataset.history_evidence_size() != dataset.source_dataset_ids_size())
        throw std::invalid_argument("historical evidence does not match dataset versions");
    auto expected = dataset.source_dataset_ids().begin();
    for (const auto& evidence : dataset.history_evidence()) {
        validate_history_evidence(evidence);
        if (evidence.dataset_id() != *expected++)
            throw std::invalid_argument("historical evidence does not match dataset versions");
    }
    validate_daily_table(dataset.daily());
    // Daily bars of the daily table are of its source and of no archive version.
    if (dataset.daily_acquired_at_ns() <= 0
        || (dataset.source_dataset_ids().empty()
            && (interval != 0 || dataset.source() != dataset.daily().daily_source())))
        throw std::invalid_argument("invalid bar dataset provenance");
    auto spec = instrument(dataset.contract());
    // Each change is of a later day than the first bar's and the change before
    // it, no later than the last bar's, and to another increment.
    auto increment = spec.price_increment;
    const auto* changed = &dataset.bars(0).trading_day();
    for (const auto& change : dataset.price_increment_changes()) {
        (void)parse_trading_date(change.trading_day());
        if (!change.has_value() || value(change.value()) <= Decimal{}
            || value(change.value()) == increment || change.trading_day() <= *changed
            || change.trading_day() > dataset.bars(dataset.bars_size() - 1).trading_day())
            throw std::invalid_argument("invalid dataset price increment change");
        increment = value(change.value());
        changed = &change.trading_day();
    }
    const std::string* before = nullptr;
    for (const auto& day : dataset.days()) {
        (void)parse_trading_date(day.trading_day());
        // A settlement price is what the exchange publishes, not a price an order
        // is placed at: the last one of a cash-settled contract is an average
        // with more places than the price increment has.
        if (!day.has_settlement_price() || value(day.settlement_price()) <= Decimal{}
            || (before && day.trading_day() <= *before))
            throw std::invalid_argument("invalid dataset settlement day");
        before = &day.trading_day();
    }
    // A day need have no bars: the contract settled on it without trading, or
    // its bars are missing. A bar needs a day: what is traded is settled.
    std::optional<MarketBar> previous;
    auto day = dataset.days().begin();
    auto change = dataset.price_increment_changes().begin();
    for (const auto& row : dataset.bars()) {
        auto bar = market_bar(row);
        // A bar is on the increment of its day.
        for (; change != dataset.price_increment_changes().end()
            && change->trading_day() <= bar.trading_day;
            ++change)
            spec.price_increment = value(change->value());
        bar.validate(spec);
        if (previous
            && (bar.timestamp_ns <= previous->timestamp_ns
                || bar.trading_day < previous->trading_day))
            throw std::invalid_argument("dataset bars must be strictly ascending");
        while (day != dataset.days().end() && day->trading_day() < bar.trading_day)
            ++day;
        if (day == dataset.days().end() || day->trading_day() != bar.trading_day)
            throw std::invalid_argument("dataset bars must fall on its settlement days");
        previous = std::move(bar);
    }
    if (dataset.revision() != bar_dataset_revision(dataset))
        throw std::invalid_argument("bar dataset revision mismatch");
}

std::vector<MarketBar> dataset_bars(const data::proto::BarDataset& dataset)
{
    validate_bar_dataset(dataset);
    std::vector<MarketBar> result;
    result.reserve(static_cast<std::size_t>(dataset.bars_size()));
    for (const auto& bar : dataset.bars())
        result.push_back(market_bar(bar));
    return result;
}

std::vector<PriceIncrementChange> dataset_price_increment_changes(
    const data::proto::BarDataset& dataset)
{
    validate_bar_dataset(dataset);
    std::vector<PriceIncrementChange> result;
    for (const auto& change : dataset.price_increment_changes())
        result.push_back({change.trading_day(), value(change.value())});
    return result;
}

std::vector<DaySettlement> dataset_days(const data::proto::BarDataset& dataset)
{
    validate_bar_dataset(dataset);
    std::vector<DaySettlement> result;
    for (const auto& day : dataset.days())
        result.push_back({day.trading_day(), value(day.settlement_price())});
    return result;
}

Json decode_bar_dataset_range(const data::proto::BarDataset& dataset)
{
    if (dataset.bars().empty())
        throw std::invalid_argument("bar dataset requires 1..200000 bars and their trading days");
    const auto& first = dataset.bars(0);
    const auto& last = dataset.bars(dataset.bars_size() - 1);
    return {
        {"count", dataset.bars_size()},
        {"first_timestamp_ns", std::to_string(first.timestamp_ns())},
        {"last_timestamp_ns", std::to_string(last.timestamp_ns())},
        {"first_day", first.trading_day()},
        {"last_day", last.trading_day()},
        {"interval_minutes", dataset.interval_minutes()},
        {"source", dataset.source()},
        {"source_dataset_ids",
            std::vector<std::string>(
                dataset.source_dataset_ids().begin(),
                dataset.source_dataset_ids().end())}
    };
}

Json decode_bar_dataset(const data::proto::BarDataset& dataset, DatasetView view)
{
    validate_bar_dataset(dataset);
    auto result = contents(dataset, view);
    result["history_evidence"] = Json::array();
    for (const auto& evidence : dataset.history_evidence())
        result["history_evidence"].push_back(decode_history_evidence(evidence));
    result.update(
        {{"revision", dataset.revision()},
            {"source", dataset.source()},
            {"source_dataset_ids", versions(dataset.source_dataset_ids())},
            {"daily", decode_daily_table(dataset.daily())},
            {"daily_acquired_at_ns", std::to_string(dataset.daily_acquired_at_ns())}});
    return result;
}

data::proto::BarDataset encode_bar_dataset(const Json& json)
{
    require_fields(
        json,
        {"revision",
            "contract",
            "price_increment_changes",
            "interval_minutes",
            "bars",
            "days",
            "source",
            "source_dataset_ids",
            "history_evidence",
            "daily",
            "daily_acquired_at_ns"});
    if (!json.at("interval_minutes").is_number_unsigned() || !json.at("bars").is_array()
        || !json.at("days").is_array() || json.at("bars").size() > max_dataset_bars
        || !json.at("price_increment_changes").is_array()
        || json.at("price_increment_changes").size() > max_dataset_bars)
        throw std::invalid_argument("unsupported bar dataset format");
    data::proto::BarDataset result;
    result.set_revision(json.at("revision").get<std::string>());
    *result.mutable_contract() = encode_contract(json.at("contract"));
    for (const auto& row : json.at("price_increment_changes")) {
        require_fields(row, {"trading_day", "value"});
        auto* change = result.add_price_increment_changes();
        change->set_trading_day(row.at("trading_day").get<std::string>());
        set(change->mutable_value(), decimal(row, "value"));
    }
    result.set_interval_minutes(json.at("interval_minutes").get<std::uint32_t>());
    for (const auto& row : json.at("bars")) {
        require_fields(
            row,
            {"trading_day", "timestamp_ns", "open", "high", "low", "close", "volume"});
        *result.add_bars() = encode_bar(
            {row.at("trading_day").get<std::string>(),
                nanoseconds(row.at("timestamp_ns")),
                decimal(row, "open"),
                decimal(row, "high"),
                decimal(row, "low"),
                decimal(row, "close"),
                decimal(row, "volume")});
    }
    for (const auto& row : json.at("days")) {
        require_fields(row, {"trading_day", "settlement_price"});
        auto* day = result.add_days();
        day->set_trading_day(row.at("trading_day").get<std::string>());
        set(day->mutable_settlement_price(), decimal(row, "settlement_price"));
    }
    result.set_source(json.at("source").get<std::string>());
    encode_versions(json.at("source_dataset_ids"), result.mutable_source_dataset_ids());
    if (!json.at("history_evidence").is_array()
        || json.at("history_evidence").size() > max_dataset_sources)
        throw std::invalid_argument("historical evidence does not match dataset versions");
    for (const auto& evidence : json.at("history_evidence"))
        *result.add_history_evidence() = encode_history_evidence(evidence);
    *result.mutable_daily() = encode_daily_table(json.at("daily"));
    result.set_daily_acquired_at_ns(nanoseconds(json.at("daily_acquired_at_ns")));
    validate_bar_dataset(result);
    return result;
}

data::proto::BarDatasetRequest encode_bar_dataset_request(const Json& json)
{
    require_fields(json, {"source_dataset_ids", "daily", "begin_day", "end_day", "contract"});
    data::proto::BarDatasetRequest result;
    encode_versions(json.at("source_dataset_ids"), result.mutable_source_dataset_ids());
    *result.mutable_daily() = encode_daily_table(json.at("daily"));
    result.set_begin_day(json.at("begin_day").get<std::string>());
    result.set_end_day(json.at("end_day").get<std::string>());
    result.set_contract(json.at("contract").get<std::string>());
    (void)decode_bar_dataset_request(result);
    return result;
}

void validate_product_bars_request(const data::proto::ProductBarsRequest& request)
{
    validate_message(request);
    validate_daily_table(request.daily());
    // A product is named as a contract is, without the delivery month.
    HistoryIdentity{request.venue(), request.product(), "2000-01"}.validate();
    if (parse_trading_date(request.begin_day()) > parse_trading_date(request.end_day()))
        throw std::invalid_argument("invalid product bars request");
}

data::proto::ProductBarsRequest encode_product_bars_request(const Json& json)
{
    require_fields(json, {"venue", "product", "begin_day", "end_day", "daily"});
    data::proto::ProductBarsRequest result;
    result.set_venue(json.at("venue").get<std::string>());
    result.set_product(json.at("product").get<std::string>());
    result.set_begin_day(json.at("begin_day").get<std::string>());
    result.set_end_day(json.at("end_day").get<std::string>());
    *result.mutable_daily() = encode_daily_table(json.at("daily"));
    validate_product_bars_request(result);
    return result;
}

Json decode_product_bars_request(const data::proto::ProductBarsRequest& request)
{
    validate_product_bars_request(request);
    return {
        {"venue", request.venue()},
        {"product", request.product()},
        {"begin_day", request.begin_day()},
        {"end_day", request.end_day()},
        {"daily", decode_daily_table(request.daily())}
    };
}

Json decode_bar_dataset_request(const data::proto::BarDatasetRequest& request)
{
    validate_message(request);
    validate_versions(request.source_dataset_ids());
    validate_daily_table(request.daily());
    for (const auto* day : {&request.begin_day(), &request.end_day()})
        if (!day->empty())
            (void)parse_trading_date(*day);
    if (!request.begin_day().empty() && !request.end_day().empty()
        && request.begin_day() > request.end_day())
        throw std::invalid_argument("dataset begin day follows end day");
    // Bars of archive versions bound the days themselves; the daily table's do not.
    if (request.source_dataset_ids().empty()
        && (request.begin_day().empty() || request.end_day().empty()))
        throw std::invalid_argument("daily bars of the daily table require a begin and an end day");
    // Named exactly as history writes it.
    if (HistoryIdentity::parse(request.contract()).key() != request.contract())
        throw std::invalid_argument("invalid dataset contract");
    return {
        {"source_dataset_ids", versions(request.source_dataset_ids())},
        {"daily", decode_daily_table(request.daily())},
        {"begin_day", request.begin_day()},
        {"end_day", request.end_day()},
        {"contract", request.contract()}
    };
}

void validate_dominant_schedule(
    const data::proto::DominantSchedule& schedule,
    std::span<const proto::Contract* const> contracts)
{
    if (schedule.rolls().empty())
        throw std::invalid_argument("invalid dominant series schedule");
    const data::proto::DominantRoll* previous = nullptr;
    for (const auto& roll : schedule.rolls()) {
        (void)parse_trading_date(roll.trading_day());
        if (roll.contract() >= contracts.size() || !roll.has_factor() || roll.factor().units() <= 0)
            throw std::invalid_argument("invalid dominant series schedule");
        if (previous) {
            const auto& contract = *contracts[roll.contract()];
            const auto& before = *contracts[previous->contract()];
            // One product, later months only, each from a later day.
            if (roll.trading_day() <= previous->trading_day() || contract.venue() != before.venue()
                || contract.product() != before.product()
                || contract.delivery_month() <= before.delivery_month())
                throw std::invalid_argument("invalid dominant series schedule");
        }
        previous = &roll;
    }
    if (previous->factor().units() != Decimal::parse("1").raw())
        throw std::invalid_argument("invalid dominant series schedule");
    const data::proto::TermPoint* before = nullptr;
    for (const auto& term : schedule.terms()) {
        (void)parse_trading_date(term.trading_day());
        const auto& month =
            contracts[dominant_roll(schedule, term.trading_day()).contract()]->delivery_month();
        if (term.trading_day() < schedule.rolls(0).trading_day()
            || (before && term.trading_day() <= before->trading_day()) || term.near().units() <= 0
            || term.far().units() <= 0 || term.far_month().size() != month.size()
            || term.far_month() <= month)
            throw std::invalid_argument("invalid dominant series schedule");
        (void)term_carry(term, month);
        before = &term;
    }
}

const data::proto::DominantRoll&
dominant_roll(const data::proto::DominantSchedule& schedule, const std::string& trading_day)
{
    const auto* current = &schedule.rolls(0);
    for (const auto& roll : schedule.rolls())
        if (roll.trading_day() <= trading_day)
            current = &roll;
    return *current;
}

Decimal term_carry(const data::proto::TermPoint& point, const std::string& near_month)
{
    // Delivery months are YYYY-MM.
    const auto months = [](const std::string& text) {
        int year = 0, month = 0;
        const auto* end = text.data() + text.size();
        if (text.size() != 7 || text[4] != '-'
            || std::from_chars(text.data(), text.data() + 4, year).ptr != text.data() + 4
            || std::from_chars(text.data() + 5, end, month).ptr != end || month < 1 || month > 12)
            throw std::invalid_argument("invalid dominant series schedule");
        return year * 12 + month;
    };
    const auto apart = months(point.far_month()) - months(near_month);
    if (apart <= 0)
        throw std::invalid_argument("invalid dominant series schedule");
    const auto spread =
        divide(value(point.near()), value(point.far()), Rounding::half_up) - Decimal::parse("1");
    return divide(
        multiply(spread, Decimal::parse("12"), Rounding::half_up),
        Decimal::parse(std::to_string(apart)),
        Rounding::half_up);
}

std::map<std::string, Decimal> term_carries(
    const data::proto::DominantSchedule& schedule,
    std::span<const proto::Contract* const> contracts)
{
    std::map<std::string, Decimal> result;
    for (const auto& term : schedule.terms())
        result.emplace(
            term.trading_day(),
            term_carry(
                term,
                contracts[dominant_roll(schedule, term.trading_day()).contract()]
                    ->delivery_month()));
    return result;
}

Json decode_term_points(const data::proto::DominantSchedule& schedule)
{
    Json terms = Json::array();
    for (const auto& term : schedule.terms())
        terms.push_back(
            {{"trading_day", term.trading_day()},
                {"near", value(term.near()).str()},
                {"far", value(term.far()).str()},
                {"far_month", term.far_month()}});
    return terms;
}

Json decode_dominant_schedule(const data::proto::DominantSchedule& schedule)
{
    Json rolls = Json::array();
    for (const auto& roll : schedule.rolls())
        rolls.push_back(
            {{"trading_day", roll.trading_day()},
                {"contract", roll.contract()},
                {"factor", value(roll.factor()).str()}});
    return rolls;
}

std::string named_dataset_revision(const data::proto::NamedDataset& value)
{
    auto canonical = value;
    canonical.clear_id();
    return sha256_bytes(canonical.SerializeAsString());
}

void validate_named_dataset(const data::proto::NamedDataset& value)
{
    validate_message(value);
    if (value.name().empty() || value.name().size() > 120 || value.name().front() == ' '
        || value.name().back() == ' '
        || std::any_of(
            value.name().begin(),
            value.name().end(),
            [](unsigned char c) {
                return c < 32 || c == 127;
            })
        || value.selections_size() < 1 || value.selections_size() > 20
        || value.content_revisions_size() != value.selections_size())
        throw std::invalid_argument("invalid saved dataset");
    std::set<std::string> contracts;
    for (int i = 0; i < value.selections_size(); ++i) {
        const auto& request = value.selections(i);
        (void)decode_bar_dataset_request(request);
        if (!contracts.insert(request.contract()).second || !digest(value.content_revisions(i)))
            throw std::invalid_argument("invalid saved dataset");
    }
    if (!digest(value.id()) || value.id() != named_dataset_revision(value))
        throw std::invalid_argument("saved dataset revision mismatch");
}

Json decode_named_dataset(const data::proto::NamedDataset& value)
{
    validate_named_dataset(value);
    Json inputs = Json::array();
    for (int i = 0; i < value.selections_size(); ++i) {
        auto input = decode_bar_dataset_request(value.selections(i));
        input["revision"] = value.content_revisions(i);
        inputs.push_back(std::move(input));
    }
    return {{"id", value.id()}, {"name", value.name()}, {"selections", std::move(inputs)}};
}

} // namespace asterion::protocol
