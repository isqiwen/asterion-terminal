import type { MessageInitShape } from "@bufbuild/protobuf";
import { decimal } from "../../src/bridge/decimal";
import type { TradingCall } from "../../src/bridge/trading";
import { Offset } from "../../src/generated/asterion/common_pb";
import type { RiskLimitsSchema } from "../../src/generated/asterion/common_pb";
import type { CatalogContract } from "../../src/generated/asterion/market_pb";
import {
  BrokerState,
  Environment,
  OrderState,
  type Account,
  type CommandSchema,
  type ContractTermsSchema,
} from "../../src/generated/asterion/trading_pb";
import { BackendError } from "../../src/i18n/errors";
import { type MessageValues, translate } from "../../src/i18n";

export const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.trading", key, values);

export const phases: Partial<Record<BrokerState, string>> = {
  [BrokerState.DISCONNECTED]: "未连接",
  [BrokerState.CONNECTING]: "连接前置",
  [BrokerState.AUTHENTICATING]: "终端认证",
  [BrokerState.LOGGING_IN]: "登录中",
  [BrokerState.CONFIRMING]: "确认结算单",
  [BrokerState.SYNCHRONIZING]: "同步账户",
  [BrokerState.READY]: "已就绪",
  [BrokerState.FAILED]: "连接失败",
};
export const statuses: Partial<Record<OrderState, string>> = {
  [OrderState.SUBMITTED]: "已报送",
  [OrderState.ACCEPTED]: "已接受",
  [OrderState.PARTIALLY_FILLED]: "部分成交",
  [OrderState.FILLED]: "已成交",
  [OrderState.CANCELLED]: "已撤单",
  [OrderState.REJECTED]: "已拒绝",
};
export const offsets: Record<Offset, string> = {
  [Offset.UNSPECIFIED]: "",
  [Offset.OPEN]: "开仓",
  [Offset.CLOSE_TODAY]: "平今",
  [Offset.CLOSE_YESTERDAY]: "平昨",
  [Offset.CLOSE]: "平仓",
};
// The counter's funds as the page and an exported day show them.
export const fundFields = [
  ["balance", "动态权益"],
  ["available", "可用资金"],
  ["margin", "占用保证金"],
  ["positionProfit", "持仓盈亏"],
  ["closeProfit", "平仓盈亏"],
  ["commission", "手续费"],
] as const;
export const environmentName = (environment: Environment | undefined) =>
  environment === Environment.REAL ? "真实资金" : "CTP 仿真";
export const key = (item: { exchange: string; symbol: string } | undefined) =>
  `${item?.exchange}.${item?.symbol}`;
export const listedKey = (row: CatalogContract) => key(row.instrument);

// One account request, built from the owner's input when sent; resolves false
// when the input or the request was refused and the panel shows why.
export type Run = (operation: TradingCall | (() => TradingCall)) => Promise<boolean>;
export type CommandAction = MessageInitShape<typeof CommandSchema>["action"];
// A trading command names the account and the policy revision the owner reviewed.
export const command = (run: Run, account: Account, action: () => CommandAction) =>
  run(() => ({
    case: "sendCommand",
    value: {
      requestId: crypto.randomUUID(),
      accountId: account.id,
      policyRevision: account.session?.policyRevision ?? "",
      action: action(),
    },
  }));

// Exchange terms of an allowed contract from the CTP contract catalog the
// market service loaded; domestic CTP futures trade whole lots.
export function policyContract(row: CatalogContract): MessageInitShape<typeof ContractTermsSchema> {
  const [, product, deliveryMonth] = row.contract.split("/");
  return {
    instrument: row.instrument,
    product,
    deliveryMonth,
    multiplier: decimal(String(row.multiplier)),
    priceTick: row.priceTick,
    lotSize: decimal("1"),
  };
}

export type LimitsDraft = {
  max_order_quantity: string;
  max_gross_quantity: string;
  max_working_orders: string;
  max_price_deviation: string;
};
export function riskLimits(draft: LimitsDraft): MessageInitShape<typeof RiskLimitsSchema> {
  const orders = draft.max_working_orders.trim();
  if (!/^\d+$/.test(orders))
    throw new BackendError("invalid_request", `invalid working order limit: ${orders}`);
  return {
    maxOrderLots: decimal(draft.max_order_quantity),
    maxGrossLots: decimal(draft.max_gross_quantity),
    maxWorkingOrders: Number(orders),
  };
}
