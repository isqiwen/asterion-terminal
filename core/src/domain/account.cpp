#include <asterion/domain/account.hpp>
#include <algorithm>
#include <stdexcept>

namespace asterion {
namespace {
const auto zero = Decimal{};
const auto one = Decimal::parse("1");
bool active(const AccountOrder& item) {
    return item.order.state() == OrderState::accepted || item.order.state() == OrderState::partially_filled;
}
const char* offset_name(Offset value) {
    switch(value) { case Offset::open: return "open"; case Offset::close_today: return "close_today"; case Offset::close_yesterday: return "close_yesterday"; }
    throw std::invalid_argument("无效开平标志");
}
const char* state_name(OrderState value) {
    switch(value) {
    case OrderState::pending: return "pending"; case OrderState::accepted: return "accepted";
    case OrderState::partially_filled: return "partially_filled"; case OrderState::filled: return "filled";
    case OrderState::cancelled: return "cancelled"; case OrderState::rejected: return "rejected";
    }
    throw std::invalid_argument("无效订单状态");
}
}
void FuturesCosts::validate() const {
    if (margin_per_lot <= zero || open_fee < zero || close_today_fee < zero || close_yesterday_fee < zero)
        throw std::invalid_argument("每手保证金必须为正，手续费不能为负");
}
Decimal FuturesCosts::fee(Offset offset) const {
    switch(offset) { case Offset::open: return open_fee; case Offset::close_today: return close_today_fee; case Offset::close_yesterday: return close_yesterday_fee; }
    throw std::invalid_argument("无效开平标志");
}
FuturesAccount::FuturesAccount(Instrument instrument, Decimal deposit, FuturesCosts costs)
    : instrument_(std::move(instrument)), costs_(costs), balance_(deposit) {
    instrument_.validate(); costs_.validate();
    if (instrument_.asset_class != AssetClass::futures || deposit <= zero || !instrument_.quantity_increment.multiple_of(one))
        throw std::invalid_argument("期货账户需要正数初始资金与整手规格");
}
AccountOrder& FuturesAccount::find(const std::string& id) {
    auto it = std::find_if(orders_.begin(), orders_.end(), [&](const auto& x) { return x.order.request().id == id; });
    if (it == orders_.end()) throw std::invalid_argument("订单不存在");
    return *it;
}
Decimal FuturesAccount::margin() const {
    Decimal result; for (const auto& lot : lots_) result = result + lot.quantity * costs_.margin_per_lot; return result;
}
Decimal FuturesAccount::unrealized() const {
    Decimal result; if (mark_ == zero) return result;
    for (const auto& lot : lots_) result = result + (lot.side == Side::buy ? mark_ - lot.price : lot.price - mark_) * lot.quantity * instrument_.multiplier;
    return result;
}
Decimal FuturesAccount::frozen() const {
    Decimal result;
    for (const auto& item : orders_) if (active(item))
        result = result + item.order.remaining_quantity() * (costs_.fee(item.offset) + (item.offset == Offset::open ? costs_.margin_per_lot : zero));
    return result;
}
Decimal FuturesAccount::available() const { return std::min(balance_, balance_ + unrealized()) - margin() - frozen(); }
Decimal FuturesAccount::closable(Side side, bool today) const {
    Decimal result;
    for (const auto& lot : lots_) if (lot.side == side && lot.today == today) result = result + lot.quantity;
    for (const auto& item : orders_) if (active(item) && item.offset != Offset::open && item.order.request().side != side && (item.offset == Offset::close_today) == today)
        result = result - item.order.remaining_quantity();
    return result;
}
void FuturesAccount::submit(LimitOrder request, Offset offset) {
    auto next = *this; next.submit_in_place(std::move(request), offset); *this = std::move(next);
}
void FuturesAccount::submit_in_place(LimitOrder request, Offset offset) {
    (void)offset_name(offset);
    if (request.side != Side::buy && request.side != Side::sell) throw std::invalid_argument("无效买卖方向");
    if (orders_.size() >= 10000) throw std::invalid_argument("模拟账户最多保留 10000 笔委托");
    for (const auto& item : orders_) if (item.order.request().id == request.id) throw std::invalid_argument("订单标识重复");
    if (request.limit_price <= zero) throw std::invalid_argument("当前期货模拟模型要求正数限价");
    Order order(request, instrument_);
    if (mark_ == zero) throw std::invalid_argument("请先回放一笔行情");
    if (offset != Offset::open && closable(request.side == Side::buy ? Side::sell : Side::buy, offset == Offset::close_today) < request.quantity)
        throw std::invalid_argument("可平持仓不足或已被其他委托冻结");
    const auto required = request.quantity * (costs_.fee(offset) + (offset == Offset::open ? costs_.margin_per_lot : zero));
    if (offset == Offset::open && required > available()) throw std::invalid_argument("可用资金不足");
    order.accept(); orders_.push_back({std::move(order), offset});
    (void)available();
}
void FuturesAccount::cancel(const std::string& id) {
    auto next = *this; next.find(id).order.cancel(); *this = std::move(next);
}
bool FuturesAccount::fill(const Fill& report) {
    for (const auto& previous : fills_) if (previous.execution_id == report.execution_id) {
        if (previous == report) return false;
        throw std::invalid_argument("成交标识冲突");
    }
    auto next = *this; next.fill_in_place(report); *this = std::move(next); return true;
}
void FuturesAccount::fill_in_place(const Fill& report) {
    if (report.price <= zero) throw std::invalid_argument("当前期货模拟模型要求正数成交价");
    auto& item = find(report.order_id);
    item.order.apply(report);
    const auto fee = report.quantity * costs_.fee(item.offset);
    fees_ = fees_ + fee; balance_ = balance_ - fee;
    if (item.offset == Offset::open) lots_.push_back({item.order.request().side, true, report.quantity, report.price});
    else {
        auto remaining = report.quantity;
        for (auto& lot : lots_) {
            if (remaining == zero) break;
            if (lot.side == item.order.request().side || lot.today != (item.offset == Offset::close_today)) continue;
            const auto amount = std::min(remaining, lot.quantity);
            const auto pnl = (lot.side == Side::buy ? report.price - lot.price : lot.price - report.price) * amount * instrument_.multiplier;
            balance_ = balance_ + pnl; realized_ = realized_ + pnl;
            lot.quantity = lot.quantity - amount; remaining = remaining - amount;
        }
        if (remaining != zero) throw std::logic_error("成交超过可平持仓");
        std::erase_if(lots_, [](const auto& lot) { return lot.quantity == zero; });
    }
    fills_.push_back(report);
}
void FuturesAccount::mark(Decimal price) {
    if (price <= zero || !price.multiple_of(instrument_.price_increment)) throw std::invalid_argument("无效标记价格");
    auto next = *this; next.mark_ = price; (void)next.available(); *this = std::move(next);
}
void FuturesAccount::settle(Decimal price) {
    auto next = *this;
    for (const auto& item : orders_) if (active(item)) throw std::invalid_argument("结算前必须撤销所有未完成委托");
    next.mark(price); const auto pnl = next.unrealized(); next.balance_ = next.balance_ + pnl; next.realized_ = next.realized_ + pnl;
    for (auto& lot : next.lots_) { lot.today = false; lot.price = price; }
    *this = std::move(next);
}
Json FuturesAccount::snapshot() const {
    Json positions = Json::array(), orders = Json::array(), fills = Json::array();
    for (const auto& lot : lots_) positions.push_back({{"side", lot.side == Side::buy ? "buy" : "sell"}, {"bucket", lot.today ? "today" : "yesterday"}, {"quantity", lot.quantity.str()}, {"basis", lot.price.str()}});
    for (const auto& item : orders_) {
        const auto& r = item.order.request();
        orders.push_back({{"id", r.id}, {"side", r.side == Side::buy ? "buy" : "sell"}, {"offset", offset_name(item.offset)}, {"quantity", r.quantity.str()}, {"limit_price", r.limit_price.str()}, {"filled", item.order.filled_quantity().str()}, {"state", state_name(item.order.state())}});
    }
    for (const auto& f : fills_) fills.push_back({{"id", f.execution_id}, {"order_id", f.order_id}, {"quantity", f.quantity.str()}, {"price", f.price.str()}});
    return {{"balance", balance_.str()}, {"equity", (balance_ + unrealized()).str()}, {"available", available().str()}, {"margin", margin().str()}, {"frozen", frozen().str()}, {"fees", fees_.str()}, {"realized", realized_.str()}, {"unrealized", unrealized().str()}, {"mark", mark_.str()}, {"positions", positions}, {"orders", orders}, {"fills", fills}};
}
}
