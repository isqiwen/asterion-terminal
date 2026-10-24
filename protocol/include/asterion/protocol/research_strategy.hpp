#pragma once
#include <asterion/research_strategy.pb.h>
#include <asterion/backtest.pb.h>
#include <asterion/foundation/serialization.hpp>
#include <string_view>

namespace asterion::protocol {

research::proto::StrategyDefinition strategy_definition(
    const backtest::proto::BacktestInput&,
    const std::string& programme,
    const std::string& risk);
void validate_strategy_definition(const research::proto::StrategyDefinition&);
std::string strategy_digest(const research::proto::StrategyDefinition&);
const resource::proto::Object& strategy_source(const research::proto::StrategyPackage&);
const resource::proto::Object& strategy_source(const research::proto::StrategyDelivery&);
void validate_strategy_logic(
    const research::proto::LogicDefinition&,
    const research::proto::StrategyDefinition&);
void validate_strategy_package(const research::proto::StrategyPackage&);
void validate_strategy_delivery(const research::proto::StrategyDelivery&);
void validate_strategy_use(const research::proto::StrategyUse&);
research::proto::StrategyUse encode_strategy_use(const Json&);
Json decode_strategy_use(const research::proto::StrategyUse&);
std::string strategy_delivery_digest(research::proto::StrategyDelivery);
resource::proto::Object strategy_package_object(const research::proto::StrategyPackage&);
resource::proto::Object strategy_delivery_object(const research::proto::StrategyDelivery&);
void validate_strategy_object(const resource::proto::Object&, std::string_view kind);
resource::proto::Object encode_strategy_object(const Json&, std::string_view kind);
Json decode_strategy_object(const resource::proto::Object&);
Json decode_strategy_package(const research::proto::StrategyPackage&);
Json decode_strategy_delivery(const research::proto::StrategyDelivery&);
Json decode_strategy_reply(
    const research::proto::StrategyReply&,
    const research::proto::StrategyRequest&,
    const resource::proto::Owner&);

} // namespace asterion::protocol
