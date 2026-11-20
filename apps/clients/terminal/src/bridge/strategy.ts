import type { TradingCall } from "./trading";
import { tradingCall } from "./trading";

// The strategy calls of trading.proto; each replies with the account's strategies.
export type StrategyCall = Extract<
  TradingCall,
  {
    case:
      | "getStrategies"
      | "publishStrategy"
      | "startStrategy"
      | "stopStrategy"
      | "pauseStrategy"
      | "flattenStrategy";
  }
>;

// Mutations are never retried; read the host again after an unknown outcome.
export function strategyRequest(account: string, operation: StrategyCall) {
  return tradingCall(account, operation as Extract<TradingCall, { case: "getStrategies" }>);
}
