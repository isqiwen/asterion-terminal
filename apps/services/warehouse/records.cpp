#include "records.hpp"
#include <stdexcept>

namespace asterion::warehouse {
namespace {

using K = Kind;
constexpr Column daily[] = {
    {"exchange", K::text},
    {"product", K::text},
    {"contract", K::text},
    {"day", K::date},
    {"open", K::number},
    {"high", K::number},
    {"low", K::number},
    {"close", K::number},
    {"settlement", K::number},
    {"previous_close", K::number},
    {"previous_settlement", K::number},
    {"volume", K::number},
    {"amount", K::number},
    {"open_interest", K::number},
    {"open_interest_change", K::number},
    {"delivery_settlement", K::number},
    {"lower_limit", K::number},
    {"upper_limit", K::number}
};
constexpr Column calendar[] = {
    {"exchange", K::text},
    {"day", K::date},
    {"open", K::boolean},
    {"previous_trading_day", K::date}
};
constexpr Column contracts[] = {
    {"exchange", K::text},
    {"contract", K::text},
    {"product", K::text},
    {"delivery_month", K::text},
    {"source_instrument", K::text},
    {"name", K::text},
    {"list_date", K::date},
    {"delist_date", K::date},
    {"last_delivery_date", K::date},
    {"multiplier", K::number},
    {"per_unit", K::number},
    {"trade_unit", K::text},
    {"quote_unit", K::text},
    {"tick_description", K::text},
    {"delivery_description", K::text},
    {"trading_hours", K::text}
};
constexpr Column minutes[] = {
    {"exchange", K::text},
    {"product", K::text},
    {"contract", K::text},
    {"day", K::date},
    {"start_ms", K::integer},
    {"partial", K::boolean},
    {"open", K::number},
    {"high", K::number},
    {"low", K::number},
    {"close", K::number},
    {"volume", K::number},
    {"average_price", K::number},
    {"open_interest", K::number}
};
constexpr Column settlement[] = {
    {"exchange", K::text},
    {"product", K::text},
    {"contract", K::text},
    {"day", K::date},
    {"long_margin_rate", K::number},
    {"short_margin_rate", K::number},
    {"fee_rate", K::number},
    {"fee_per_lot", K::number},
    {"close_today_fee_rate", K::number},
    {"close_today_fee_per_lot", K::number}
};
constexpr Column price_ticks[] = {
    {"exchange", K::text},
    {"product", K::text},
    {"contract", K::text},
    {"effective_from", K::date},
    {"effective_until", K::date},
    {"price_tick", K::price},
    {"source_url", K::text}
};
constexpr Column warehouse_receipts[] = {
    {"exchange", K::text},
    {"product", K::text},
    {"day", K::date},
    {"warehouse", K::text},
    {"warehouse_id", K::text},
    {"warehouse_type", K::text},
    {"area", K::text},
    {"crop_year", K::text},
    {"grade", K::text},
    {"brand", K::text},
    {"origin", K::text},
    {"unit", K::text},
    {"converted", K::text},
    {"previous_volume", K::number},
    {"volume", K::number},
    {"volume_change", K::number},
    {"forecast_volume", K::number},
    {"premium", K::number},
    {"published_ms", K::integer},
    {"observed_ms", K::integer}
};
constexpr Column member_positions[] = {
    {"exchange", K::text},
    {"product", K::text},
    {"contract", K::text},
    {"day", K::date},
    {"member", K::text},
    {"volume", K::number},
    {"volume_change", K::number},
    {"long_position", K::number},
    {"long_change", K::number},
    {"short_position", K::number},
    {"short_change", K::number},
    {"published_ms", K::integer},
    {"observed_ms", K::integer}
};
constexpr Column inventory[] = {
    {"exchange", K::text},
    {"product", K::text},
    {"day", K::date},
    {"area", K::text},
    {"warehouse", K::text},
    {"grade", K::text},
    {"unit", K::text},
    {"previous_total", K::number},
    {"previous_receipts", K::number},
    {"total", K::number},
    {"receipts", K::number},
    {"total_change", K::number},
    {"receipts_change", K::number},
    {"previous_capacity", K::number},
    {"capacity", K::number},
    {"capacity_change", K::number},
    {"published_ms", K::integer},
    {"observed_ms", K::integer}
};

} // namespace

std::span<const Column> columns(std::string_view table)
{
    if (table == "daily")
        return daily;
    if (table == "calendar")
        return calendar;
    if (table == "contracts")
        return contracts;
    if (table == "minutes")
        return minutes;
    if (table == "settlement")
        return settlement;
    if (table == "price_ticks")
        return price_ticks;
    if (table == "warehouse_receipts")
        return warehouse_receipts;
    if (table == "member_positions")
        return member_positions;
    if (table == "inventory")
        return inventory;
    throw std::invalid_argument("unknown warehouse table");
}

Row row(const CalendarDay& r)
{
    return {r.exchange, r.day, r.open, r.previous_trading_day};
}

Row row(const Contract& r)
{
    return {
        r.exchange,
        r.contract,
        r.product,
        r.delivery_month,
        r.source_instrument,
        r.name,
        r.list_date,
        r.delist_date,
        r.last_delivery_date,
        r.multiplier,
        r.per_unit,
        r.trade_unit,
        r.quote_unit,
        r.tick_description,
        r.delivery_description,
        r.trading_hours
    };
}

Row row(const DailyBar& r)
{
    return {
        r.exchange,
        r.product,
        r.contract,
        r.day,
        r.open,
        r.high,
        r.low,
        r.close,
        r.settlement,
        r.previous_close,
        r.previous_settlement,
        r.volume,
        r.amount,
        r.open_interest,
        r.open_interest_change,
        r.delivery_settlement,
        r.lower_limit,
        r.upper_limit
    };
}

Row row(const MinuteBar& r)
{
    return {
        r.exchange,
        r.product,
        r.contract,
        r.day,
        r.start_ms,
        r.partial,
        r.open,
        r.high,
        r.low,
        r.close,
        r.volume,
        r.average_price,
        r.open_interest
    };
}

Row row(const Settlement& r)
{
    return {
        r.exchange,
        r.product,
        r.contract,
        r.day,
        r.long_margin_rate,
        r.short_margin_rate,
        r.fee_rate,
        r.fee_per_lot,
        r.close_today_fee_rate,
        r.close_today_fee_per_lot
    };
}

Row row(const PriceTick& r)
{
    return {
        r.exchange,
        r.product,
        r.contract,
        r.effective_from,
        r.effective_until,
        r.price_tick,
        r.source_url
    };
}

Row row(const WarehouseReceipt& r)
{
    return {
        r.exchange,
        r.product,
        r.day,
        r.warehouse,
        r.warehouse_id,
        r.warehouse_type,
        r.area,
        r.crop_year,
        r.grade,
        r.brand,
        r.origin,
        r.unit,
        r.converted,
        r.previous_volume,
        r.volume,
        r.volume_change,
        r.forecast_volume,
        r.premium,
        r.published_ms,
        r.observed_ms
    };
}

Row row(const MemberPosition& r)
{
    return {
        r.exchange,
        r.product,
        r.contract,
        r.day,
        r.member,
        r.volume,
        r.volume_change,
        r.long_position,
        r.long_change,
        r.short_position,
        r.short_change,
        r.published_ms,
        r.observed_ms
    };
}

Row row(const Inventory& r)
{
    return {
        r.exchange,
        r.product,
        r.day,
        r.area,
        r.warehouse,
        r.grade,
        r.unit,
        r.previous_total,
        r.previous_receipts,
        r.total,
        r.receipts,
        r.total_change,
        r.receipts_change,
        r.previous_capacity,
        r.capacity,
        r.capacity_change,
        r.published_ms,
        r.observed_ms
    };
}

CalendarDay read_calendar_day(const Row& r)
{
    return {
        .exchange = std::get<std::string>(r[0]),
        .day = std::get<std::string>(r[1]),
        .open = std::get<bool>(r[2]),
        .previous_trading_day = r[3]
    };
}

Contract read_contract(const Row& r)
{
    const auto text = [&](std::size_t at) {
        return std::get<std::string>(r[at]);
    };
    return {
        .exchange = text(0),
        .contract = text(1),
        .product = text(2),
        .delivery_month = text(3),
        .source_instrument = text(4),
        .name = r[5],
        .list_date = text(6),
        .delist_date = text(7),
        .last_delivery_date = r[8],
        .multiplier = r[9],
        .per_unit = r[10],
        .trade_unit = r[11],
        .quote_unit = r[12],
        .tick_description = r[13],
        .delivery_description = r[14],
        .trading_hours = r[15]
    };
}

} // namespace asterion::warehouse
