#pragma once
#include <asterion/trading.pb.h>
#include <asterion/foundation/serialization.hpp>

namespace asterion::protocol {

inline constexpr std::size_t max_trading_facts_bytes = 4 * 1024 * 1024;
void validate_trading_facts_reference(const resource::proto::Object&);
void validate_trading_facts(const proto::TradingFactsCapture&);
void validate_trading_facts_receipt(const proto::TradingFactsReceipt&);
void validate_trading_facts_status(const proto::TradingFactsStatus&);
Json decode_trading_facts_status(const proto::TradingFactsStatus&);
proto::TradingFactsCapture freeze_trading_facts(
    const resource::proto::Owner&,
    const std::string& instance,
    const std::string& id,
    std::int64_t captured_at_ms,
    const proto::LiveSnapshot&);
Json decode_trading_facts(const proto::TradingFactsCapture&);

} // namespace asterion::protocol
